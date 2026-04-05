//这个头文件定义了一个名为 UAvatarGrpcSubsystem 的类，它是 Unreal 的游戏实例子系统（UGameInstanceSubsystem）
//子系统在游戏实例创建时自动创建，在游戏实例销毁时自动销毁，非常适合做全局性的功能（比如 gRPC 客户端管理、麦克风录制、数据分发）
//这个类负责：发送音频文件/文本到 gRPC 服务器。 实时麦克风采集并流式发送。 缓存最新的响应帧，并广播给监听者（如 AVirtualHumanMicActor）。 将面部和身体数据应用到 MetaHuman 的网格和控制绑定上

#pragma once  //防止头文件被多次包含

#include "CoreMinimal.h"  //包含 Unreal 核心基础头文件（提供 FString、TArray、UObject 等）
#include "Subsystems/GameInstanceSubsystem.h"  //包含游戏实例子系统的基类头文件。要继承 UGameInstanceSubsystem，所以需要知道它的定义

#include <atomic>  //包含 C++11 标准库的原子操作头文件。提供 std::atomic<bool> 类型，用于多线程安全地读写布尔值（比如麦克风运行标志）
#include <thread>  //包含 C++ 标准库的线程头文件。提供 std::thread 类，用来创建和管理后台线程（用于麦克风实时采集）

#include "AvatarGrpcTypes.h"  //包含我们之前定义的数据结构（FAvatarRuntimeFrame 等）

#include "AvatarGrpcSubsystem.generated.h"  //包含 Unreal 反射系统生成的头文件

class UControlRigComponent;  //告诉编译器这两个类是存在的，但暂时不需要知道它们的完整定义。我们会在函数参数中使用这些类的指针，所以先声明一下，避免编译错误。
class USkeletalMeshComponent;
namespace avatar { class StreamResponse; }  //前置声明 avatar::StreamResponse 类（protobuf 消息）。我们有一个静态方法 ResponseToRuntimeFrame 会用到这个类型，但头文件里不需要包含整个 avatar_stream.pb.h，这样可以减少编译依赖
namespace grpc { class ClientContext; }  //前置声明 grpc::ClientContext 类。在 MicWorker 中我们需要取消 gRPC 调用，因此要保存 ClientContext 指针，但头文件里不需要包含 gRPC 头文件

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAvatarFrameUpdated, FAvatarRuntimeFrame, Frame);  //声明一个动态多播委托，名为 FOnAvatarFrameUpdated，它带一个参数（FAvatarRuntimeFrame 类型）。
                                                                                                 //这是一个“事件”类型。当新的一帧数据到达时，可以广播这个事件，任何监听了这个事件的对象（比如 AVirtualHumanMicActor）都会收到通知并执行自己的处理函数
                                                                                                 //DYNAMIC：允许在蓝图中使用
                                                                                                 //MULTICAST：多个对象可以同时监听
                                                                                                 //DELEGATE：委托，相当于 C# 的事件或其它语言的回调列表
                                                                                                 //OneParam：带一个参数

UCLASS()  //这是一个 Unreal 的类宏，告诉 UHT 这个类要参与反射系统。即这个类可以被蓝图使用，可以拥有 UFUNCTION、UPROPERTY 等特性
class VIRTUALHUMANGRPCBRIDGE_API UAvatarGrpcSubsystem : public UGameInstanceSubsystem  //VIRTUALHUMANGRPCBRIDGE_API：导出宏，使得这个类可以被其他模块使用
                                                                                       //UAvatarGrpcSubsystem：类名，U 前缀表示继承自 UObject
                                                                                       //: public UGameInstanceSubsystem：公开继承自游戏实例子系统
{
	GENERATED_BODY()  //Unreal 必需的宏，用于生成反射相关的代码。放在类的最开头，让 UHT 知道这里要插入自动生成的函数声明

public:
	UFUNCTION(BlueprintCallable, Category="VirtualHuman|gRPC")  //UFUNCTION(BlueprintCallable, ...)：这个函数可以在蓝图中调用
	                                                            //Category="VirtualHuman|gRPC"：在蓝图编辑器中，这个函数会归类到 VirtualHuman 下的 gRPC 子分类。
	void Run(const FString& Input);  //void Run(const FString& Input)：输入一个字符串（可以是 WAV 文件路径或文本，由内部逻辑自动判断）

	UFUNCTION(BlueprintCallable, Category="VirtualHuman|gRPC")
	void RunText(const FString& Text);  //强制以文本模式发送输入（即使传入的字符串看起来像文件路径，也当作文本处理）

	UFUNCTION(BlueprintCallable, Category="VirtualHuman|gRPC")
	void StartMicRealtime();  //开始实时麦克风采集，并将音频流式发送给服务器

	UFUNCTION(BlueprintCallable, Category="VirtualHuman|gRPC")
	void StopMicRealtime();  //停止麦克风实时采集

	UFUNCTION(BlueprintCallable, Category="VirtualHuman|gRPC")
	bool IsMicRealtimeRunning() const { return bMicRunning.load(); }  //查询麦克风是否正在运行
	                                                                  //const：不会修改对象状态
	                                                                  //bMicRunning.load()：原子地读取 bMicRunning 的值

	UFUNCTION(BlueprintCallable, Category="VirtualHuman|gRPC")
	bool GetLatestFrame(FAvatarRuntimeFrame& OutFrame) const;  //获取最新收到的一帧数据（从服务器返回的最近一次结果）

	UFUNCTION(BlueprintCallable, Category="VirtualHuman|gRPC")
	void ApplyFaceFrameToMesh(USkeletalMeshComponent* FaceMesh, const FAvatarFaceFrame& FaceFrame, float WeightScale = 1.0f) const;  //将面部表情数据应用到角色的骨骼网格体（USkeletalMeshComponent）上
	                                                                                                                                 //FaceMesh：目标网格组件（通常是 MetaHuman 的面部网格）
	                                                                                                                                 //FaceFrame：包含表情控制点名字和权重的数据结构
	                                                                                                                                 //WeightScale：缩放权重（例如设为 0.5 可以减弱表情强度）

	UFUNCTION(BlueprintCallable, Category="VirtualHuman|gRPC")
	void ApplyBodyFrameToControlRig(UControlRigComponent* ControlRig, const FAvatarBodyFrame& BodyFrame, float WeightScale = 1.0f) const;  //将身体动作数据应用到 Control Rig 组件上

	UPROPERTY(BlueprintAssignable, Category="VirtualHuman|gRPC")  //UPROPERTY(BlueprintAssignable, ...)：这是一个属性，可以在蓝图中绑定事件
	FOnAvatarFrameUpdated OnFrameUpdated;  //当新的一帧数据到达时，这个事件会被广播。你在蓝图中可以把这个事件连到某个自定义事件上，就能实时响应数据更新

	virtual void Deinitialize() override;  //重写（override）基类的 Deinitialize 函数。当子系统被销毁时，Unreal 会调用这个函数。即做一些清理工作，比如停止麦克风线程

private:
	void RunInternal(const FString& Input, bool bForceText);  //内部函数，Run 和 RunText 都调用它，通过 bForceText 参数区分是强制文本还是自动判断。实际干活的函数，对外公开的 Run/RunText 只是包装
	void MicWorker(FString Address);  //在后台线程中运行的函数，负责麦克风采集、构建 WAV 包、发送 gRPC 流、接收响应
	void CacheAndBroadcastFrame(const FAvatarRuntimeFrame& Frame);  //将新帧存入 LatestFrame（线程安全），然后广播 OnFrameUpdated 事件

	static FAvatarRuntimeFrame ResponseToRuntimeFrame(const avatar::StreamResponse& Resp);  //将 protobuf 的 StreamResponse 转换成 Unreal 的 FAvatarRuntimeFrame
	static FTransform AxisAngleToTransform(const FVector& AxisAngleRadians);  //将轴角表示（旋转轴 + 角度，弧度制）转换为 Unreal 的 FTransform（四元数旋转）
	static TArray<FString> BuildControlNameVariants(const FString& BaseName);  //根据一个基础名称（如 "head"）生成多个可能的 Control Rig 控制点名称变体（例如 "head_ctrl"、"CTRL_head" 等）。
	static TArray<FString> BuildBodyJointNames();  //返回一个预定义的关节名称列表（如 "root", "pelvis", "spine_01" 等）。这些是常见的 MetaHuman 骨骼名称，用于将服务器返回的数值数组按顺序映射到各个关节。

private:
	UPROPERTY(EditAnywhere, Category="VirtualHuman|gRPC")  //UPROPERTY(EditAnywhere, ...)：这个属性可以在编辑器详情面板中修改
	FString ServerAddress = TEXT("127.0.0.1:50051");  //gRPC 服务器的地址和端口，默认本地 127.0.0.1:50051。即可以直接在虚幻编辑器里改服务器地址，不需要重新编译

	mutable FCriticalSection FrameMutex;
	FAvatarRuntimeFrame LatestFrame;  //存储最新收到的一帧数据

	mutable FCriticalSection MicContextMutex;  //保护 MicContext 指针的互斥锁。当主线程想要取消麦克风线程的 gRPC 调用时，需要安全地访问 MicContext
	grpc::ClientContext* MicContext = nullptr;  //指向 gRPC 客户端上下文的指针。用于在停止麦克风时主动取消正在进行的 RPC 调用，避免线程阻塞。及可以用来打断正在等待服务器响应的线程

	std::atomic<bool> bMicRunning { false };  //原子布尔变量，表示麦克风线程是否应该继续运行。即多个线程可以安全地读取和修改这个标志，用来控制循环的退出
	std::thread MicThread;  //C++ 线程对象，用于运行 MicWorker 函数。即麦克风采集是在后台线程进行的，不阻塞游戏主线程
};
