//这个头文件定义了 FAvatarGrpcClient 类，它是 gRPC 客户端的封装，负责向服务器发送请求（音频文件或文本）并接收响应（面部、身体数据等）

#pragma once  //防止头文件被多次包含。不管你在多少个 .cpp 里 #include 这个头文件，编译器只会处理一次

#include "CoreMinimal.h"  //包含 Unreal 引擎的核心基础头文件（提供 FString、TArray 等常用类型）
#include "AvatarGrpcTypes.h"  //包含我们自己定义的数据结构（FAvatarRuntimeFrame 等）

namespace avatar  //前置声明（forward declaration）avatar::StreamRequest 类。头文件里只使用了 StreamRequest 的引用（作为函数参数），不需要包含整个 .pb.h，这样可以减少编译依赖，加快编译速度
{
	class StreamRequest;
}


class VIRTUALHUMANGRPCBRIDGE_API FAvatarGrpcClient  //这个类是这个插件的“公共接口”，其他模块（比如 UAvatarGrpcSubsystem）可以使用它
                                                    //VIRTUALHUMANGRPCBRIDGE_API：导出宏，表示这个类会被其他模块使用（跨 DLL 边界）
                                                    //FAvatarGrpcClient：类名，F 前缀是 Unreal 的习惯，表示普通类
{
public:
	explicit FAvatarGrpcClient(const FString& InAddress);  //构造函数：创建客户端对象时需要传入服务器地址（如 "127.0.0.1:50051"）
	                                                       //explicit：防止隐式类型转换（比如不能直接用字符串赋值给对象）
	                                                       //const FString&：传入一个常量引用，避免复制字符串

	bool LoadFile(const FString& Path, TArray<uint8>& OutData, FString& OutError);  //辅助函数，把文件（比如 WAV）加载成字节数组
	                                                                                //Path：文件路径
	                                                                                //OutData：输出参数，存放文件内容
	                                                                                //OutError：输出参数，如果失败则存放错误信息

	bool InferWavFile(  //发送一个 WAV 音频文件到 gRPC 服务器，并接收返回的结果。即把音频文件发给 AI 服务，让它分析并返回驱动虚拟人的数据
		const FString& WavPath,  //WavPath：WAV 文件路径
		FAvatarRuntimeFrame& OutFrame,  //OutFrame：输出参数，存放服务器返回的完整数据（面部、身体等）
		FString& OutError,  //OutError：错误信息
		bool bWantAudio = false);  //bWantAudio：是否请求服务器返回音频（默认为 false）

	bool InferText(  //发送一段文本到 gRPC 服务器，返回结果。即输入文字，服务器会合成语音并返回面部/身体动画
		const FString& Text,
		FAvatarRuntimeFrame& OutFrame,
		FString& OutError,
		bool bWantAudio = true);  //但 bWantAudio 默认为 true（文本转语音后返回音频）

private:
	bool InferStreamRequest(  //真正执行 gRPC 流式请求的内部函数。InferWavFile 和 InferText 都会构造一个 StreamRequest 然后调用它
		const avatar::StreamRequest& Request,  //已经填好的 protobuf 请求对象
		FAvatarRuntimeFrame& OutFrame,  //输出结果
		FString& OutError);  //错误信息

private:
	FString ServerAddress;  //存储服务器地址（构造函数传入）
};
