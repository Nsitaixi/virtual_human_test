#include "AvatarGrpcClient.h"  //包含对应的类声明头文件

#include "Misc/FileHelper.h"  //Unreal 的文件辅助工具，提供 LoadFileToArray 等函数。用来读写文件
#include "Misc/Paths.h"  //处理文件路径（如判断文件是否存在、拼接路径等）。用来操作路径字符串

THIRD_PARTY_INCLUDES_START  //这是一个宏，定义在 Unreal 核心模块中。在包含第三方库的头文件之前，保存当前编译器的某些警告/宏设置，然后屏蔽一些常见的第三方库警告（比如 4251、4244 等）。

#pragma push_macro("check")  //将当前这几个宏（check、verify、TEXT、PI）的当前定义保存起来，然后后面可以重新定义它们而不影响原来的。
#pragma push_macro("verify")  //Unreal 自己定义了一些宏（比如 check 是断言，TEXT 是字符串宽窄转换），而 gRPC 或 Windows SDK 也可能定义同名宏，会造成冲突。先 push 保存旧值，之后可以 pop 恢复。
#pragma push_macro("TEXT")
#pragma push_macro("PI")

#undef check  //取消这些宏的当前定义。现在可以安全地包含第三方库的头文件了，因为宏冲突已经被临时屏蔽。
#undef verify
#undef TEXT
#undef PI

#include <grpcpp/grpcpp.h>  //gRPC C++ 核心头文件
#include "avatar_stream.pb.h"  //由 avatar_stream.proto 生成的普通 protobuf 消息头文件
#include "avatar_stream.grpc.pb.h"  //由 avatar_stream.proto 生成的 gRPC 服务头文件（包含 AvatarService::Stub 等）

#pragma pop_macro("PI")  //从之前保存的栈中恢复 PI、TEXT、verify、check 这四个宏的旧定义，让 Unreal 的代码恢复正常
#pragma pop_macro("TEXT")
#pragma pop_macro("verify")
#pragma pop_macro("check")

THIRD_PARTY_INCLUDES_END  //这是一个宏，与 THIRD_PARTY_INCLUDES_START 对应，恢复编译警告设置（比如重新开启之前关闭的警告）

namespace  //定义一个匿名命名空间。在这个大括号里面的函数和变量，只能在这个 .cpp 文件内部使用，其他文件看不到。相当于“私有辅助函数”。避免与其它模块中的同名函数冲突。
{
	static bool LoadFileToBytes(const FString& Path, TArray<uint8>& OutBytes, FString& OutError)  //static：这个函数只在当前 .cpp 文件中可见
	                                                                                              //const FString& Path：要加载的文件路径
	                                                                                              //TArray<uint8>& OutBytes：输出参数，存放文件内容的字节数组
	                                                                                              //FString& OutError：输出参数，存放错误信息
	{
		OutBytes.Reset();  //清空输出数组，保证里面没有旧数据

		if (!FPaths::FileExists(Path))  //FPaths::FileExists 是 Unreal 的函数，检查文件是否存在
		{
			OutError = FString::Printf(TEXT("File not found: %s"), *Path);  //FString::Printf 类似 C 语言的 printf，生成格式化的字符串
			                                                                //TEXT("...") 是 Unreal 的宏，用于处理宽字符（兼容 Unicode）
			return false;  //返回失败
		}

		if (!FFileHelper::LoadFileToArray(OutBytes, *Path))  //FFileHelper::LoadFileToArray 是 Unreal 提供的函数，把整个文件读入 TArray<uint8>
		                                                     //第一个参数是输出的字节数组，第二个参数是文件路径（需要 *Path 转换）
		{
			OutError = FString::Printf(TEXT("Failed to load file: %s"), *Path);
			return false;
		}

		if (OutBytes.Num() <= 0)  //检查读出来的字节数是否小于等于 0（空文件）
		                          //Num() 返回数组元素个数
		{
			OutError = FString::Printf(TEXT("Empty file: %s"), *Path);
			return false;
		}

		return true;
	}

	static FAvatarRuntimeFrame ResponseToRuntimeFrame(const avatar::StreamResponse& InResp)  //这个函数把 gRPC 服务器返回的 protobuf 消息转换成 Unreal 能直接使用的 FAvatarRuntimeFrame 结构体。它负责所有类型转换（UTF-8 → TCHAR，std::string → TArray<uint8> 等）
	                                                                                         //输入：一个 protobuf 的 StreamResponse 对象（来自 gRPC 服务器）
	                                                                                         //输出：转换后的 Unreal 结构体 FAvatarRuntimeFrame
	{
		FAvatarRuntimeFrame OutFrame;  //创建一个空的结果帧
		OutFrame.Seq = InResp.seq();  //protobuf 生成的类有 seq() 方法，返回序列号。直接赋值
		OutFrame.Status = UTF8_TO_TCHAR(InResp.status().c_str());  //InResp.status() 返回 std::string（UTF-8 编码）
		                                                           //.c_str() 得到 const char*
		                                                           //UTF8_TO_TCHAR 是 Unreal 的宏，将 UTF-8 字符串转换为 Unreal 的 TCHAR 字符串（通常是 UTF-16）
		OutFrame.Emotion = UTF8_TO_TCHAR(InResp.emotion().c_str());  //同理处理 Emotion

		OutFrame.Face.Names.Reset();  //清空数组，避免残留旧数据
		OutFrame.Face.Weights.Reset();
		for (const auto& Name : InResp.face().names())  //遍历 protobuf 返回的 names 列表（repeated string）
		                                                //const auto& 是 C++11 的自动类型推导，避免拷贝
		{
			OutFrame.Face.Names.Add(UTF8_TO_TCHAR(Name.c_str()));  //将每个名字转换后添加到 Unreal 的字符串数组。
		}
		for (float Weight : InResp.face().weights())  //遍历权重列表（repeated float），直接添加到 Weights 数组
		{
			OutFrame.Face.Weights.Add(Weight);
		}

		OutFrame.Body.ChannelNames.Reset();  //身体部分类似
		OutFrame.Body.Values.Reset();
		for (const auto& ChannelName : InResp.body().channel_names())
		{
			OutFrame.Body.ChannelNames.Add(UTF8_TO_TCHAR(ChannelName.c_str()));
		}
		for (float Value : InResp.body().values())
		{
			OutFrame.Body.Values.Add(Value);
		}
		OutFrame.Body.Format = UTF8_TO_TCHAR(InResp.body().format().c_str());

		OutFrame.AudioWav.Reset();  //清空音频字节数组
		if (!InResp.wav().empty())  //如果服务器返回了音频数据（wav 字段非空）
		{
			const std::string& Wav = InResp.wav();  //获取音频数据的引用（避免复制）
			OutFrame.AudioWav.Append(reinterpret_cast<const uint8*>(Wav.data()), Wav.size());  //Wav.data() 返回 const char*，但我们需要 const uint8*，所以用 reinterpret_cast 转换类型
			                                                                                   //Wav.size() 是字节数
			                                                                                   //Append 把这段数据加到 AudioWav 数组末尾
		}

		return OutFrame;  //返回填充好的结构体
	}
}

FAvatarGrpcClient::FAvatarGrpcClient(const FString& InAddress)  //创建客户端对象时，把服务器地址记下来
	: ServerAddress(InAddress)  //初始化列表，用传入的地址初始化成员变量 ServerAddress
{
}

bool FAvatarGrpcClient::LoadFile(const FString& Path, TArray<uint8>& OutData, FString& OutError)
{
	return LoadFileToBytes(Path, OutData, OutError);  //直接调用匿名命名空间里的 LoadFileToBytes
}

bool FAvatarGrpcClient::InferStreamRequest(
	const avatar::StreamRequest& Request,
	FAvatarRuntimeFrame& OutFrame,
	FString& OutError)
{
	grpc::ChannelArguments ChannelArgs;  //一个配置对象，用来设置通道参数
	ChannelArgs.SetMaxSendMessageSize(50 * 1024 * 1024);  //设置最大发送消息大小为 50 MB。因为音频文件可能较大
	ChannelArgs.SetMaxReceiveMessageSize(50 * 1024 * 1024);  //同理，接收消息大小限制

	auto Channel = grpc::CreateCustomChannel(  //返回一个 std::shared_ptr<grpc::Channel>
		TCHAR_TO_UTF8(*ServerAddress),  //服务器地址，用 TCHAR_TO_UTF8 把 FString 转成 UTF-8 字符串
		grpc::InsecureChannelCredentials(),  //表示不使用 SSL/TLS（明文通信）
		ChannelArgs);  //通道参数

	if (!Channel)  //如果 !Channel（通道创建失败），记录错误并返回 false
	{
		OutError = FString::Printf(TEXT("Failed to create gRPC channel: %s"), *ServerAddress);
		return false;
	}

	auto Stub = avatar::AvatarService::NewStub(Channel);  //AvatarService 是在 .proto 文件中定义的服务
	                                                      //NewStub 创建一个客户端存根（stub），通过它调用远程方法
	                                                      //auto 自动推导类型为 std::unique_ptr<avatar::AvatarService::Stub>
	grpc::ClientContext Context;  //用于维护一次 RPC 调用的上下文（超时、元数据等）

	auto Stream = Stub->StreamInfer(&Context);  //调用服务端的 StreamInfer 方法，该方法在 proto 中定义为 rpc StreamInfer(stream StreamRequest) returns (stream StreamResponse);
	                                            //返回一个 std::unique_ptr<grpc::ClientReaderWriter<StreamRequest, StreamResponse>> 之类的流对象
	if (!Stream)  //如果流创建失败，记录错误
	{
		OutError = TEXT("Failed to create gRPC stream");
		return false;
	}

	if (!Stream->Write(Request))  //如果向服务器发送一个 StreamRequest 消息失败
	{
		OutError = TEXT("gRPC write failed");  //如果流创建失败，记录错误
		Stream->WritesDone();  //通知服务器“客户端不会再有写入”
		Stream->Finish();  //等待 RPC 完成并获取最终状态
		return false;
	}

	Stream->WritesDone();  //如果发送成功，也要调用 WritesDone，表示“我已经写完所有请求了”。因为本例只发一个请求，然后关闭写端

	bool bGotAnyResponse = false;  //bGotAnyResponse 标志是否收到了至少一个响应
	OutFrame = FAvatarRuntimeFrame();
	OutFrame.Seq = -1;  //OutFrame 先重置为默认值（Seq = -1）

	avatar::StreamResponse Resp;  //
	while (Stream->Read(&Resp))  //循环读取服务器发来的每个 StreamResponse。Read 在有数据时返回 true，当服务器关闭流或出错时返回 false
	{
		bGotAnyResponse = true;
		OutFrame = ResponseToRuntimeFrame(Resp);  //每读到一个响应，就调用 ResponseToRuntimeFrame 转换成 Unreal 结构体，并保存到 OutFrame
	}

	const grpc::Status Status = Stream->Finish();  //Stream->Finish() 返回 RPC 的最终状态（成功或失败）
	if (!Status.ok())  //Status.ok() 为 true 表示调用成功
	{
		OutError = UTF8_TO_TCHAR(Status.error_message().c_str());  //如果失败，把错误信息（UTF-8）转换成 FString 存入 OutError
		return false;
	}

	if (!bGotAnyResponse)  //如果根本没有收到任何响应，也视为失败
	{
		OutError = TEXT("No response from server");
		return false;
	}

	return true;  //检查整个通话是否成功，如果服务器报错或没返回数据，就算失败
}

bool FAvatarGrpcClient::InferWavFile(  //读取指定路径的 WAV 文件，构造一个请求，然后发送给服务器并返回结果
	const FString& WavPath,
	FAvatarRuntimeFrame& OutFrame,
	FString& OutError,
	bool bWantAudio)
{
	TArray<uint8> WavBytes;  //先调用 LoadFileToBytes 把 WAV 文件读成字节数组
	if (!LoadFileToBytes(WavPath, WavBytes, OutError))
	{
		return false;
	}

	avatar::StreamRequest Req;  //创建一个 StreamRequest protobuf 对象
	Req.set_seq(0);  //序列号（这里固定为 0，实际可以递增）
	Req.set_mode(avatar::REQUEST_MODE_AUDIO);  //模式为音频文件
	Req.set_text("");  //清空文本字段
	Req.set_want_audio(bWantAudio);  //是否请求服务器返回音频
	Req.set_want_face(true);  //请求面部数据
	Req.set_want_body(true);  //请求身体数据
	Req.set_wav(reinterpret_cast<const char*>(WavBytes.GetData()), WavBytes.Num());  //把 WAV 字节数组传给 protobuf 的 bytes 字段。注意将 uint8* 转为 const char*

	return InferStreamRequest(Req, OutFrame, OutError);  //调用 InferStreamRequest 发送请求并获取结果
}

bool FAvatarGrpcClient::InferText(  //把用户输入的文本发给服务器，服务器会分析文本并返回面部/身体数据（也可能返回合成语音）
	const FString& Text,
	FAvatarRuntimeFrame& OutFrame,
	FString& OutError,
	bool bWantAudio)
{
	avatar::StreamRequest Req;
	Req.set_seq(0);
	Req.set_mode(avatar::REQUEST_MODE_TEXT);  //模式为 REQUEST_MODE_TEXT
	Req.set_text(TCHAR_TO_UTF8(*Text));
	Req.set_want_audio(bWantAudio);
	Req.set_want_face(true);
	Req.set_want_body(true);

	return InferStreamRequest(Req, OutFrame, OutError);
}
