#include "AvatarGrpcSubsystem.h"  //包含对应的头文件，这样编译器就知道类的声明
#include "AvatarGrpcClient.h"  //包含我们之前写的 gRPC 客户端类。RunInternal 里创建了 FAvatarGrpcClient 对象，所以需要这个头文件

#include "Async/Async.h"  //包含 Unreal 的异步任务系统。即可以在后台线程执行 gRPC 调用，然后通过 AsyncTask 回到游戏线程更新数据
#include "Components/SkeletalMeshComponent.h"  //包含骨骼网格体组件，用于 ApplyFaceFrameToMesh
#include "HAL/PlatformProcess.h"  //包含平台相关的进程功能，这里用到了 FPlatformProcess::Sleep（让线程休眠一小段时间）
#include "Interfaces/VoiceCapture.h"  //包含语音捕获接口，用于麦克风采集
#include "Modules/ModuleManager.h"  //包含模块管理器，用于动态加载 Voice 模块
#include "VoiceModule.h"  //包含语音模块，提供了 FVoiceModule 和 CreateVoiceCapture 函数

#include "ControlRigComponent.h"  //包含 Control Rig 组件，用于 ApplyBodyFrameToControlRig

THIRD_PARTY_INCLUDES_START

#pragma push_macro("check")
#pragma push_macro("verify")
#pragma push_macro("TEXT")
#pragma push_macro("PI")

#undef check
#undef verify
#undef TEXT
#undef PI

#include <grpcpp/grpcpp.h>
#include "avatar_stream.pb.h"
#include "avatar_stream.grpc.pb.h"

#pragma pop_macro("PI")
#pragma pop_macro("TEXT")
#pragma pop_macro("verify")
#pragma pop_macro("check")

THIRD_PARTY_INCLUDES_END

namespace
{
	static void WriteUInt16LE(TArray<uint8>& Out, uint16 Value)  //写一个16位整数到字节数组（小端序）。这个函数把一个 16 位整数拆成两个字节，按“小端”顺序（先放低字节，后放高字节）写进字节数组。WAV 文件格式要求整数按小端序存储。
	                                                             //TArray<uint8>& Out：一个字节数组（输出参数），函数会把转换后的字节追加到它末尾
	                                                             //uint16 Value：要写入的 16 位无符号整数（范围 0 ~ 65535）
	{
		Out.Add(static_cast<uint8>(Value & 0xFF));  //Value & 0xFF：取出 Value 的低 8 位（最低的一个字节）
		                                            //static_cast<uint8>(...)：将结果转换为 uint8 类型（0~255）
		                                            //Out.Add(...)：将这个字节追加到数组末尾
		Out.Add(static_cast<uint8>((Value >> 8) & 0xFF));  //Value >> 8：右移 8 位，把原来的高 8 位移到低 8 位位置
		                                                   //& 0xFF：只保留低 8 位（实际上已经是低 8 位，但为了清晰）
		                                                   //转换成 uint8 后追加到数组
	}

	static void WriteUInt32LE(TArray<uint8>& Out, uint32 Value)  //写一个32位整数到字节数组（小端序）
	{
		Out.Add(static_cast<uint8>(Value & 0xFF));
		Out.Add(static_cast<uint8>((Value >> 8) & 0xFF));
		Out.Add(static_cast<uint8>((Value >> 16) & 0xFF));
		Out.Add(static_cast<uint8>((Value >> 24) & 0xFF));
	}

	static TArray<uint8> BuildWavBytes(  //将 PCM 原始数据加上 WAV 文件头，构建成完整的 WAV 字节数组
		const uint8* PcmData,  //PcmData：指向 PCM 原始数据的指针（每个样本是 16 位有符号整数，连续存放）
		uint32 PcmBytes,  //PcmBytes：PCM 数据的字节数（不是样本数）
		uint16 NumChannels,  //NumChannels：声道数（1 = 单声道，2 = 立体声）
		uint32 SampleRate,  //SampleRate：采样率（如 16000 Hz）
		uint16 BitsPerSample)  //BitsPerSample：每个样本的位数（通常是 16）
	{
		TArray<uint8> Out;  //创建一个空的字节数组，用于存放最终的 WAV 数据
		Out.Reserve(44 + PcmBytes);  //Reserve 提前分配足够的内存空间（44 字节的文件头 + PCM 数据大小），避免后续添加时多次重新分配内存，提高性能

		const uint16 BlockAlign = static_cast<uint16>((NumChannels * BitsPerSample) / 8);  //BlockAlign：块对齐字节数，表示一帧（一个采样点包含所有声道）的字节数
		                                                                                   //公式：(声道数 * 位深) / 8。例如：单声道 16 位 → (1 * 16)/8 = 2 字节
		const uint32 ByteRate = SampleRate * BlockAlign;  //ByteRate：每秒的字节数（采样率 × 块对齐）。例如 16000 Hz × 2 字节 = 32000 字节/秒
		const uint32 RiffChunkSize = 36 + PcmBytes;  //WAV 文件的 RIFF chunk 大小 = 整个文件长度减去 8 字节（"RIFF" 标识和这个字段本身）。标准公式是 36 + PCM数据字节数

		Out.Append(reinterpret_cast<const uint8*>("RIFF"), 4);  //Append("RIFF", 4)：追加 4 个字节 'R','I','F','F'
		WriteUInt32LE(Out, RiffChunkSize);  //写入 chunk 大小（小端序）
		Out.Append(reinterpret_cast<const uint8*>("WAVE"), 4);  //追加 'W','A','V','E' 格式标识

		Out.Append(reinterpret_cast<const uint8*>("fmt "), 4);  //写入 "fmt " 子块（格式信息）
		WriteUInt32LE(Out, 16);  //fmt 块的大小（后面跟 16 字节）
		WriteUInt16LE(Out, 1);  //音频格式（1 = PCM）
		WriteUInt16LE(Out, NumChannels);  //声道数
		WriteUInt32LE(Out, SampleRate);  //采样率
		WriteUInt32LE(Out, ByteRate);  //字节率
		WriteUInt16LE(Out, BlockAlign);  //块对齐
		WriteUInt16LE(Out, BitsPerSample);  //位深

		Out.Append(reinterpret_cast<const uint8*>("data"), 4);  //"data" 标识
		WriteUInt32LE(Out, PcmBytes);  //接着写入数据块的大小（即 PCM 数据的字节数）

		if (PcmBytes > 0)  //如果 PCM 数据不为空，就把原始数据追加到末尾
		{
			Out.Append(PcmData, static_cast<int32>(PcmBytes));
		}

		return Out;  //返回完整的 WAV 字节数组
	}

	static FAvatarRuntimeFrame ToRuntimeFrame(const avatar::StreamResponse& Resp)  //将 protobuf 的 StreamResponse 转换成 Unreal 的 FAvatarRuntimeFrame
	                                                                               //读取 seq（序列号）、status、emotion（使用 UTF8_TO_TCHAR 转换编码）
	                                                                               //清空面部数组，然后遍历 face().names() 和 face().weights()，分别添加到 Names 和 Weights 数组中
	                                                                               //清空身体数组，遍历 body().channel_names() 和 body().values()，添加到 ChannelNames 和 Values，再设置 Format
	                                                                               //如果有音频数据（wav 字段非空），将其拷贝到 AudioWav 字节数组
	                                                                               //返回填充好的 Frame
	{
		FAvatarRuntimeFrame Frame;
		Frame.Seq = Resp.seq();
		Frame.Status = UTF8_TO_TCHAR(Resp.status().c_str());
		Frame.Emotion = UTF8_TO_TCHAR(Resp.emotion().c_str());

		Frame.Face.Names.Reset();
		Frame.Face.Weights.Reset();
		for (const auto& Name : Resp.face().names())
		{
			Frame.Face.Names.Add(UTF8_TO_TCHAR(Name.c_str()));
		}
		for (float Weight : Resp.face().weights())
		{
			Frame.Face.Weights.Add(Weight);
		}

		Frame.Body.ChannelNames.Reset();
		Frame.Body.Values.Reset();
		for (const auto& ChannelName : Resp.body().channel_names())
		{
			Frame.Body.ChannelNames.Add(UTF8_TO_TCHAR(ChannelName.c_str()));
		}
		for (float Value : Resp.body().values())
		{
			Frame.Body.Values.Add(Value);
		}
		Frame.Body.Format = UTF8_TO_TCHAR(Resp.body().format().c_str());

		Frame.AudioWav.Reset();
		if (!Resp.wav().empty())
		{
			const std::string& Wav = Resp.wav();
			Frame.AudioWav.Append(reinterpret_cast<const uint8*>(Wav.data()), Wav.size());
		}

		return Frame;
	}

	static bool IsValidBodyControlRig(UControlRigComponent* Rig)  //检查 ControlRig 组件是否有效且可执行。即判断这个 Control Rig 能不能用来设置控制点
	{
		return Rig != nullptr && Rig->CanExecute();  //Rig != nullptr：指针不为空
		                                             //Rig->CanExecute()：Control Rig 组件是否处于可以执行的状态（比如已经初始化、拥有有效的骨骼等）
	}

	static bool TrySetControlTransformByNames(  //尝试用一组候选名字设置 ControlRig 上的控制点变换（旋转/位移）。只要找到一个能用的名字就设置并返回成功。SetControlTransform 即使名字不存在也不会报错（只是忽略），所以遍历所有候选是安全的。
	                                            //MetaHuman 的不同版本或不同 Control Rig 资产中，控制点的命名规则可能不一样。这个函数挨个试，总能找到正确的名字并设置数值
		UControlRigComponent* ControlRig,  //ControlRig：目标 Control Rig 组件
		const TArray<FString>& CandidateNames,  //CandidateNames：候选名称列表，例如 ["head", "head_ctrl", "CTRL_head"]
		const FTransform& Transform)  //Transform：要设置的变换（通常是旋转）
	{
		if (!IsValidBodyControlRig(ControlRig))  //检查 Control Rig 是否有效，无效则返回 false
		{
			return false;
		}

		bool bApplied = false;  //初始化 bApplied = false
		for (const FString& Candidate : CandidateNames)  //遍历每个候选名称
		{
			if (Candidate.IsEmpty())  //跳过空字符串
			{
				continue;
			}

			ControlRig->SetControlTransform(FName(*Candidate), Transform, EControlRigComponentSpace::LocalSpace);  //调用 ControlRig->SetControlTransform，传入名称（转换为 FName）、变换值、以及空间（本地空间）
			bApplied = true;  //将 bApplied 设为 true（只要有一个成功设置，就算成功）
		}
		return bApplied;  //返回 bApplied
	}

	static bool TrySetControlFloatByNames(  //与上面类似，但用于设置浮点类型的控制值（而不是变换）。例如某些 Control Rig 使用单独的浮点控制来驱动混合变形或简单参数。
		UControlRigComponent* ControlRig,
		const TArray<FString>& CandidateNames,
		float Value)
	{
		if (!IsValidBodyControlRig(ControlRig))
		{
			return false;
		}

		bool bApplied = false;
		for (const FString& Candidate : CandidateNames)
		{
			if (Candidate.IsEmpty())
			{
				continue;
			}

			ControlRig->SetControlFloat(FName(*Candidate), Value);
			bApplied = true;
		}
		return bApplied;
	}
}

FAvatarRuntimeFrame UAvatarGrpcSubsystem::ResponseToRuntimeFrame(const avatar::StreamResponse& Resp)  //这是一个静态成员函数（static），它只是调用了匿名命名空间里的 ToRuntimeFrame 函数，并直接返回结果
{
	return ToRuntimeFrame(Resp);
}

FTransform UAvatarGrpcSubsystem::AxisAngleToTransform(const FVector& AxisAngleRadians)  //将“轴角”（Axis-Angle）表示法转换成 Unreal 的 FTransform（变换）
                                                                                        //输入：一个 FVector，其中 X, Y, Z 分量表示旋转轴的方向，向量的长度表示旋转的角度（弧度制）
                                                                                        //输出：一个 FTransform，只包含旋转（位置为零，缩放为 1）
{
	const float Angle = AxisAngleRadians.Length();  //计算向量的长度（即旋转的角度，单位弧度）。FVector::Length() 返回 sqrt(x²+y²+z²)
	if (Angle <= KINDA_SMALL_NUMBER)  //KINDA_SMALL_NUMBER 是 Unreal 定义的一个非常小的正数（约 1e-8）。如果角度极小（接近 0），说明几乎没有旋转，直接返回单位变换（FTransform::Identity），避免除以零
	{
		return FTransform::Identity;
	}

	const FVector Axis = AxisAngleRadians / Angle;  //将向量除以长度，得到单位方向向量（长度为 1），即旋转轴的方向。例如：输入 (0, 0, 1.57)，长度 1.57（约 90°），轴就是 (0, 0, 1)。
	const FQuat RotQuat(Axis, Angle);  //FQuat 是四元数（Quaternion），是表示旋转的数学工具，没有万向锁问题。FQuat 的构造函数接受一个轴（单位向量）和一个角度（弧度），生成对应的旋转四元数
	return FTransform(RotQuat, FVector::ZeroVector, FVector::OneVector);  //创建一个 FTransform。旋转：RotQuat。平移：FVector::ZeroVector（零向量）。缩放：FVector::OneVector（各分量均为 1，即不缩放）
}

TArray<FString> UAvatarGrpcSubsystem::BuildControlNameVariants(const FString& BaseName)  //根据一个基础名称（例如 "head"）生成一组可能的 Control Rig 控制点名称变体
{
	TArray<FString> Out;  //创建一个空的字符串数组
	Out.Add(BaseName);  //直接加入原始名称，例如 "head"
	Out.Add(BaseName + TEXT("_ctrl"));  //加 _ctrl 后缀，例如 "head_ctrl"
	Out.Add(TEXT("CTRL_") + BaseName);  //加 CTRL_ 前缀，例如 "CTRL_head"
	Out.Add(TEXT("CTRL_") + BaseName + TEXT("_CTRL"));  //加前缀和后缀，例如 "CTRL_head_CTRL"
	Out.Add(TEXT("C_") + BaseName);  //加 C_ 前缀，例如 "C_head"
	Out.Add(TEXT("C_") + BaseName + TEXT("_CTRL"));  //例如 "C_head_CTRL"
	Out.Add(BaseName + TEXT("_C"));  //加 _C 后缀，例如 "head_C"
	Out.Add(TEXT("ctrl_") + BaseName);  //小写前缀 ctrl_，例如 "ctrl_head"
	Out.Add(BaseName + TEXT("_CTRL"));  //大写后缀 _CTRL，例如 "head_CTRL"
	return Out;  //返回数组
}

TArray<FString> UAvatarGrpcSubsystem::BuildBodyJointNames()  //返回一个预定义的关节名称列表，顺序固定。这是一个人体骨骼的标准顺序，从根骨骼到脚趾。服务器按这个顺序输出数值，我们就按这个顺序去设置 Control Rig
{
	return {
		TEXT("root"),
		TEXT("pelvis"),
		TEXT("spine_01"),
		TEXT("spine_02"),
		TEXT("spine_03"),
		TEXT("neck_01"),
		TEXT("head"),
		TEXT("clavicle_l"),
		TEXT("upperarm_l"),
		TEXT("lowerarm_l"),
		TEXT("hand_l"),
		TEXT("clavicle_r"),
		TEXT("upperarm_r"),
		TEXT("lowerarm_r"),
		TEXT("hand_r"),
		TEXT("thigh_l"),
		TEXT("calf_l"),
		TEXT("foot_l"),
		TEXT("ball_l"),
		TEXT("thigh_r"),
		TEXT("calf_r"),
		TEXT("foot_r"),
		TEXT("ball_r")
	};
}

void UAvatarGrpcSubsystem::Run(const FString& Input)  //对外公开的蓝图可调用函数，接收一个字符串 Input，然后调用内部函数 RunInternal，第二个参数 bForceText = false，false 表示“不要强制文本模式”，即让 RunInternal 自动判断 Input 是文件路径还是文本
{
	RunInternal(Input, false);
}

void UAvatarGrpcSubsystem::RunText(const FString& Text)  //强制以文本模式处理输入，即使传入的字符串看起来像文件路径，也当作文本发送
{
	RunInternal(Text, true);
}

void UAvatarGrpcSubsystem::RunInternal(const FString& Input, bool bForceText)  //异步函数，它在后台线程中执行 gRPC 调用，然后在游戏线程中广播结果
{
	const FString Address = ServerAddress;  //ddress：将成员变量 ServerAddress（服务器地址）复制到局部变量，因为后面的 lambda 会捕获它，而成员变量可能在异步执行期间发生变化（虽然概率低）
	TWeakObjectPtr<UAvatarGrpcSubsystem> WeakThis(this);  //WeakThis：创建一个弱指针指向当前子系统对象。异步任务可能在子系统已经被销毁后才执行（比如用户退出游戏）。弱指针可以安全地检查对象是否还存在，避免访问野指针
	                                                      //TWeakObjectPtr 是 Unreal 的智能指针，不会阻止对象被销毁

	Async(EAsyncExecution::ThreadPool, [WeakThis, Address, Input, bForceText]()  //Async：Unreal 的异步任务启动函数
	                                                                             //EAsyncExecution::ThreadPool：表示任务在线程池中执行（后台线程）
	                                                                             //[ ... ]()：C++ Lambda 表达式（匿名函数），捕获了 WeakThis、Address、Input、bForceText 的副本
	{
		FAvatarGrpcClient Client(Address);  //创建一个 FAvatarGrpcClient 对象，传入服务器地址
		FAvatarRuntimeFrame RuntimeFrame;  //RuntimeFrame：存放服务器返回的数据
		FString Error;  //Error：存放错误信息
		bool bOk = false;  //bOk：是否成功

		if (bForceText)  //如果 bForceText 为真，调用 InferText（发送文本）
		{
			bOk = Client.InferText(Input, RuntimeFrame, Error, true);
		}
		else  //否则调用 InferWavFile（发送音频文件）。注意 bWantAudio 参数：文本模式请求音频（true），文件模式不请求音频（false）
		{
			bOk = Client.InferWavFile(Input, RuntimeFrame, Error, false);  //
		}

		if (!bOk)  //如果调用失败（网络错误、服务器错误等），使用 AsyncTask 将错误信息打印到游戏线程的日志中
		{
			AsyncTask(ENamedThreads::GameThread, [Error]()  //ENamedThreads::GameThread 指定任务在游戏主线程执行
			{
				UE_LOG(LogTemp, Error, TEXT("gRPC Failed: %s"), *Error);  //UE_LOG 是 Unreal 的日志宏，LogTemp 是临时日志类别，Error 是错误级别
			});
			return;  //然后 return 退出 Lambda，不再继续
		}

		AsyncTask(ENamedThreads::GameThread, [WeakThis, RuntimeFrame]()  //再次使用 AsyncTask 回到游戏线程，因为操作 UObject（如广播委托、更新 UI）必须在游戏线程进行
		{
			if (!WeakThis.IsValid())  //先检查 WeakThis 是否仍然有效（子系统没有被销毁）
			{
				return;
			}

			WeakThis->CacheAndBroadcastFrame(RuntimeFrame);  //调用 CacheAndBroadcastFrame(RuntimeFrame)：将帧缓存并广播事件（OnFrameUpdated）

			UE_LOG(LogTemp, Log, TEXT("gRPC OK Seq=%d Face=%d Body=%d Status=%s Emotion=%s AudioBytes=%d"),  //打印成功日志，显示序号、面部权重数、身体数值数、状态、情感、音频字节数
				RuntimeFrame.Seq,
				RuntimeFrame.Face.Weights.Num(),
				RuntimeFrame.Body.Values.Num(),
				*RuntimeFrame.Status,
				*RuntimeFrame.Emotion,
				RuntimeFrame.AudioWav.Num());
		});
	});
}

bool UAvatarGrpcSubsystem::GetLatestFrame(FAvatarRuntimeFrame& OutFrame) const  //线程安全地获取最新缓存的响应
{
	FScopeLock Lock(&FrameMutex);  //FScopeLock 是 Unreal 的自动锁对象。在构造函数中锁定互斥量（FrameMutex），在析构函数中自动解锁。保护 LatestFrame 不被其他线程同时读写（避免数据竞争）
                                   //&FrameMutex 是取互斥锁的地址
	OutFrame = LatestFrame;  //将缓存的帧复制到输出参数（拷贝，不是引用）
	return OutFrame.Seq >= 0;  //如果 Seq 大于等于 0，表示有有效数据（因为初始值是 -1）
}

void UAvatarGrpcSubsystem::CacheAndBroadcastFrame(const FAvatarRuntimeFrame& Frame)  //新帧存入缓存（线程安全），然后广播 OnFrameUpdated 事件，通知所有监听者
{
	{  //定义了一个作用域。让 FScopeLock 在这个作用域结束时自动解锁，而不是等到函数结束。这样可以尽快释放锁，减少锁持有的时间
		FScopeLock Lock(&FrameMutex);  //锁定
		LatestFrame = Frame;  //更新缓存帧（拷贝）
	}

	OnFrameUpdated.Broadcast(Frame);  //OnFrameUpdated 是在头文件中声明的动态多播委托
	                                  //Broadcast 会调用所有绑定到该委托的回调函数，并将 Frame 作为参数传递。例如 AVirtualHumanMicActor 中绑定的 HandleFrameUpdated 就会被触发
}

void UAvatarGrpcSubsystem::ApplyFaceFrameToMesh(  //将面部表情控制数据（morph targets）应用到角色的面部网格体上，让模型做出表情
	USkeletalMeshComponent* FaceMesh,  //FaceMesh：面部网格体组件（USkeletalMeshComponent）
	const FAvatarFaceFrame& FaceFrame,  //FaceFrame：包含表情控制点名字和权重的结构体
	float WeightScale) const  //WeightScale：权重缩放系数，可以整体减弱或增强表情强度（默认 1.0）
{
	if (!FaceMesh)  //如果网格体无效，直接返回
	{
		return;
	}

	const int32 Num = FMath::Min(FaceFrame.Names.Num(), FaceFrame.Weights.Num());  //取名字数组和权重数组的较小长度，防止越界（理论上应该相等，但防御性编程）
	for (int32 Index = 0; Index < Num; ++Index)  //
	{
		const float Weight = FMath::Clamp(FaceFrame.Weights[Index] * WeightScale, 0.0f, 1.0f);  //先乘以缩放系数
		                                                                                        //FMath::Clamp 将结果限制在 0 到 1 之间（morph target 权重通常不能超出这个范围）
		FaceMesh->SetMorphTarget(FName(*FaceFrame.Names[Index]), Weight, false);  //FName(*FaceFrame.Names[Index])：将 FString 转换为 FName（Unreal 的字符串标识符，效率更高）
		                                                                          //SetMorphTarget 设置指定 morph target 的权重
		                                                                          //最后一个参数 false：表示不立即更新（通常会在下一帧统一更新，性能更好）
	}
}

void UAvatarGrpcSubsystem::ApplyBodyFrameToControlRig(  //应用身体动作
	UControlRigComponent* ControlRig,
	const FAvatarBodyFrame& BodyFrame,
	float WeightScale) const
{
	if (!IsValidBodyControlRig(ControlRig) || BodyFrame.Values.Num() <= 0)  //没有有效的 Control Rig 或者没有数据，就不做任何事
	                                                                        //IsValidBodyControlRig 检查 ControlRig 是否有效且可执行
	                                                                        //BodyFrame.Values.Num() <= 0：没有数值数据则返回
	{
		return;
	}

	const bool bHasChannelNames = BodyFrame.ChannelNames.Num() == BodyFrame.Values.Num() && BodyFrame.ChannelNames.Num() > 0;  //bHasChannelNames：是否有通道名称，且名称数量与数值数量相同（说明每个数值对应一个命名通道）
	const bool bLooksLikeAxisAngle = BodyFrame.Format.Contains(TEXT("axis_angle"), ESearchCase::IgnoreCase)  //bLooksLikeAxisAngle：通过 Format 字段判断数据是否是“轴角”格式（字符串中是否包含 "axis_angle" 等关键字）
		|| BodyFrame.Format.Contains(TEXT("axisangle"), ESearchCase::IgnoreCase)
		|| BodyFrame.Format.Contains(TEXT("axis-angle"), ESearchCase::IgnoreCase);

	if (bHasChannelNames && !bLooksLikeAxisAngle)  //按通道名设置（每个值单独控制）
	{
		for (int32 Index = 0; Index < BodyFrame.Values.Num(); ++Index)
		{
			const FString& ChannelName = BodyFrame.ChannelNames[Index];  //取通道名（如 "head_rotation_x"）
			const float Value = BodyFrame.Values[Index] * WeightScale;  //乘以权重缩放
			const TArray<FString> Candidates = BuildControlNameVariants(ChannelName);  //生成名字变体（BuildControlNameVariants）
			TrySetControlFloatByNames(ControlRig, Candidates, Value);  //调用 TrySetControlFloatByNames 尝试设置浮点控制值
		}

		return;
	}

	if (BodyFrame.Values.Num() >= 3)  //按轴角数组设置（传统方式）。先确保数值至少 3 个（一个完整的轴角需要 3 个分量）
	{
		const TArray<FString> JointNames = BuildBodyJointNames();  //JointNames 是预定义的关节顺序列表（如 "root", "pelvis", ...）
		const int32 ValuesPerJoint = 3;  //ValuesPerJoint = 3：每个关节 3 个浮点数（x, y, z 轴角）
		const int32 JointCount = FMath::Min(JointNames.Num(), BodyFrame.Values.Num() / ValuesPerJoint);  //JointCount：最多能取多少个完整关节（防止数组越界）

		for (int32 JointIndex = 0; JointIndex < JointCount; ++JointIndex)  //循环每个关节
		{
			const int32 ValueOffset = JointIndex * ValuesPerJoint;  //ValueOffset：从数组的哪个位置开始取
			const FVector AxisAngleRadians(  //取出 3 个值构成一个 FVector（轴角）
				BodyFrame.Values[ValueOffset + 0] * WeightScale,
				BodyFrame.Values[ValueOffset + 1] * WeightScale,
				BodyFrame.Values[ValueOffset + 2] * WeightScale);

			const FTransform JointTransform = AxisAngleToTransform(AxisAngleRadians);  //调用 AxisAngleToTransform 转换成 FTransform（旋转）
			const TArray<FString> Candidates = BuildControlNameVariants(JointNames[JointIndex]);  //根据关节名称生成变体
			TrySetControlTransformByNames(ControlRig, Candidates, JointTransform);  //调用 TrySetControlTransformByNames 设置控制点的变换（通常是旋转）
		}
	}
}

void UAvatarGrpcSubsystem::StartMicRealtime()  //启动一个后台线程，开始实时采集麦克风音频并发送给服务器
{
	bool Expected = false;  //用于原子比较交换的预期值
	if (!bMicRunning.compare_exchange_strong(Expected, true))  //compare_exchange_strong 是原子操作。这是线程安全的“启动一次”检查
	                                                           //如果当前 bMicRunning 的值等于 Expected（即 false），则将其设置为 true，并返回 true
	                                                           //如果不等（说明已经是 true），则不做修改，并将 Expected 更新为当前值，返回 false
	{
		return;
	}

	const FString Address = ServerAddress;  //复制服务器地址，避免 lambda 捕获成员变量时可能因对象销毁而悬空
	TWeakObjectPtr<UAvatarGrpcSubsystem> WeakThis(this);  //弱指针，避免 lambda 持有强引用导致子系统无法销毁
	MicThread = std::thread([WeakThis, Address]()  //创建一个 C++ 线程，lambda 作为线程函数
	{
		if (WeakThis.IsValid())  //在线程中先检查弱指针是否有效，如果有效则调用 MicWorker(Address)
		{
			WeakThis->MicWorker(Address);  //MicWorker 是实际的麦克风处理函数（后面会解释）
		}
	});
}

void UAvatarGrpcSubsystem::StopMicRealtime()  //停止麦克风线程，并等待线程结束。即先设置“停止标志”，然后主动打断 gRPC 调用，最后等待后台线程真正结束，这样不会留下僵尸线程。
{
	if (!bMicRunning.exchange(false))
	{
		return;
	}

	{  //锁定 MicContextMutex，安全访问 MicContext
		FScopeLock Lock(&MicContextMutex);
		if (MicContext)  //MicContext 是 gRPC 的 ClientContext 指针，在 MicWorker 中被创建
		{
			MicContext->TryCancel();  //ryCancel() 主动取消正在阻塞的 RPC 调用，使 Stream->Read() 或 Stream->Write() 尽快返回错误，从而让 MicWorker 线程退出循环
		}
	}

	if (MicThread.joinable())  //joinable() 判断线程是否还在运行且可以等待
	{
		MicThread.join();  //join() 阻塞当前线程（通常是游戏主线程），直到 MicWorker 线程完全退出。确保线程资源被正确回收
	}
}

void UAvatarGrpcSubsystem::Deinitialize()  //子系统销毁时的清理。确保在子系统销毁前，麦克风线程被正确停止，避免资源泄漏或崩溃
{
	StopMicRealtime();
	Super::Deinitialize();  //调用父类的清理函数
}

void UAvatarGrpcSubsystem::MicWorker(FString Address)  //成员函数实现，Address 是服务器地址（从 StartMicRealtime 传入）
{
	const int32 SampleRate = 16000;  //采样率 16kHz（16,000 次/秒），语音识别常用的质量
	const int32 NumChannels = 1;  //单声道（麦克风通常一个声道）
	const int32 BitsPerSample = 16;  //每个样本 16 位（2 字节），CD 音质

	const float ChunkSeconds = 0.5f;  //每个音频块的长度 0.5 秒
	const uint32 BytesPerFrame = (NumChannels * BitsPerSample) / 8;  //每一帧（一个采样点）的字节数 = 声道数 × 位深 / 8 = 1×16/8 = 2 字节
	const uint32 ChunkFrames = FMath::Max(1, FMath::RoundToInt(SampleRate * ChunkSeconds));  //每个块包含的帧数 = 采样率 × 秒数 = 16000 × 0.5 = 8000 帧
	                                                                                         //FMath::RoundToInt 四舍五入取整，FMath::Max(1, ...) 确保至少 1 帧
	const uint32 ChunkBytes = ChunkFrames * BytesPerFrame;  //每个块的字节数 = 8000 × 2 = 16000 字节（约 15.6 KB）

	int32 Seq = 0;  //序列号，每次发送递增
	TArray<uint8> PendingPcm;  //累积的原始 PCM 数据缓冲区，未达到 ChunkBytes 时暂存

	auto Channel = grpc::CreateChannel(  //与 InferStreamRequest 类似，创建到服务器的网络通道
		TCHAR_TO_UTF8(*Address),  //将 FString 转为 UTF-8 C 字符串
		grpc::InsecureChannelCredentials());  //无加密连接

	auto Stub = avatar::AvatarService::NewStub(Channel);  //创建 gRPC 客户端存根

	grpc::ClientContext* Context = new grpc::ClientContext();  //在堆上分配一个 ClientContext，用于管理这次 RPC 调用
	{  //用互斥锁保护，将 Context 指针存入成员变量 MicContext，以便 StopMicRealtime 能够取消它
		FScopeLock Lock(&MicContextMutex);
		MicContext = Context;
	}

	auto Stream = Stub->StreamInfer(Context);  //开启双向流，返回 ClientReaderWriter 对象

	if (!Stream)  //如果流创建失败，记录日志，将 bMicRunning 设为 false（通知循环退出），清理 Context 并返回
	{
		UE_LOG(LogTemp, Error, TEXT("Stream create failed"));
		bMicRunning.store(false);
		FScopeLock Lock(&MicContextMutex);
		delete MicContext;
		MicContext = nullptr;
		return;
	}

	FVoiceModule& VoiceModule = FModuleManager::LoadModuleChecked<FVoiceModule>("Voice");  //加载 Unreal 的 Voice 模块，它提供了麦克风捕获功能
	                                                                                       //LoadModuleChecked 如果失败会抛出异常（但通常不会）
	TSharedPtr<IVoiceCapture> VoiceCapture = VoiceModule.CreateVoiceCapture(TEXT(""), SampleRate, NumChannels);  //创建一个语音捕获对象，参数：设备名（空字符串表示默认设备）、采样率、声道数

	if (!VoiceCapture.IsValid())  //检查是否创建成功
	{
		UE_LOG(LogTemp, Error, TEXT("CreateVoiceCapture failed"));
		bMicRunning.store(false);
		FScopeLock Lock(&MicContextMutex);
		delete MicContext;
		MicContext = nullptr;
		return;
	}

	VoiceCapture->Start();  //开始捕获音频

	const TWeakObjectPtr<UAvatarGrpcSubsystem> WeakThis(this);

	while (bMicRunning.load())  //主循环：采集、打包、发送、接收
	{
		uint32 AvailableBytes = 0;  //用于接收可用的字节数

		if (VoiceCapture->GetCaptureState(AvailableBytes) == EVoiceCaptureState::Ok && AvailableBytes > 0)  //查询当前有多少新音频数据可用。如果状态正常且有数据，进入内部
		{
			TArray<uint8> Temp;  //创建临时数组，大小等于可用字节数，但不初始化（提高性能）
			Temp.SetNumUninitialized(AvailableBytes);

			uint32 ReadBytes = 0;  //实际读取的字节数
			if (VoiceCapture->GetVoiceData(Temp.GetData(), Temp.Num(), ReadBytes))  //将麦克风数据拷贝到 Temp 中，ReadBytes 返回实际读取的字节数
			{
				PendingPcm.Append(Temp.GetData(), static_cast<int32>(ReadBytes));  //将新数据追加到 PendingPcm 缓冲区末尾
			}
		}

		while (bMicRunning.load() && PendingPcm.Num() >= static_cast<int32>(ChunkBytes))  //当积累够一个块时，发送
		{
			TArray<uint8> Wav = BuildWavBytes(  //从 PendingPcm 的开头取出 ChunkBytes 字节的 PCM 数据，加上 WAV 头，生成完整的 WAV 数据块
				PendingPcm.GetData(),
				ChunkBytes,
				NumChannels,
				SampleRate,
				BitsPerSample);

			avatar::StreamRequest Req;
			Req.set_seq(Seq++);  //设置序列号并递增
			Req.set_mode(avatar::REQUEST_MODE_MIC);  //模式为麦克风实时
			Req.set_want_audio(false);  //不请求服务器返回音频（因为实时场景不需要）
			Req.set_want_face(true);  //需要面部数据
			Req.set_want_body(true);  //需要身体数据
			Req.set_wav(reinterpret_cast<const char*>(Wav.GetData()), Wav.Num());  //将 WAV 字节数组设置到 wav 字段

			if (!Stream->Write(Req))  //如果写入失败，记录错误并设置停止标志
			{
				UE_LOG(LogTemp, Error, TEXT("Mic stream write failed"));
				bMicRunning.store(false);
				break;
			}

			avatar::StreamResponse Resp;
			if (Stream->Read(&Resp))  //阻塞等待服务器返回一个响应（服务器通常会对每个音频块返回一帧驱动数据）
			{
				const FAvatarRuntimeFrame Frame = ToRuntimeFrame(Resp);  //如果成功，调用 ToRuntimeFrame 转换成 Unreal 结构体

				AsyncTask(ENamedThreads::GameThread, [WeakThis, Frame]()  //使用 AsyncTask 切回游戏线程，调用 CacheAndBroadcastFrame 更新缓存并广播事件
				{
					if (!WeakThis.IsValid())
					{
						return;
					}

					WeakThis->CacheAndBroadcastFrame(Frame);
					UE_LOG(LogTemp, Log, TEXT("Mic Seq=%d Face=%d Body=%d Status=%s Emotion=%s"),  //打印日志
						Frame.Seq,
						Frame.Face.Weights.Num(),
						Frame.Body.Values.Num(),
						*Frame.Status,
						*Frame.Emotion);
				});
			}
			else  //如果 Read 返回 false（服务器关闭流或出错），设置停止标志
			{
				bMicRunning.store(false);
				break;
			}

			PendingPcm.RemoveAt(0, static_cast<int32>(ChunkBytes), EAllowShrinking::No);  //从缓冲区头部删除已经发送的 ChunkBytes 字节，剩下的数据保留
			                                                                              //EAllowShrinking::No 不收缩内存，避免频繁分配
		}

		FPlatformProcess::Sleep(0.01f);  //让线程休眠 10 毫秒，避免空转占用过多 CPU
	}

	VoiceCapture->Stop();  //停止音频捕获
	VoiceCapture->Shutdown();  //释放资源

	if (bMicRunning.load())  //如果 bMicRunning 仍然为 true，WritesDone() 告诉服务器不再写入，Finish() 等待最终状态
	{
		Stream->WritesDone();
		Stream->Finish();
	}
	else  //如果 bMicRunning 为 false（被外部停止），则先 TryCancel() 取消调用，再关闭
	{
		Context->TryCancel();
		Stream->WritesDone();
		Stream->Finish();
	}

	{  //在互斥锁保护下删除 ClientContext 并置空，释放内存
		FScopeLock Lock(&MicContextMutex);
		delete MicContext;
		MicContext = nullptr;
	}
}

