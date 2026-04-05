//这个文件定义了数据结构（相当于“数据容器”），用来在蓝图和 C++ 之间传递面部表情、身体动作等信息。它相当于定义了一些“盒子”，每个盒子有特定的格子（字段），用来存放从 gRPC 服务器收到的结果。

#pragma once  //防止重复包含

#include "CoreMinimal.h"  //包含 Unreal 核心类型
#include "AvatarGrpcTypes.generated.h"  //包含 Unreal 的反射代码生成头文件。这个文件是 Unreal 自动生成的（在你编译时），它包含了与 UHT（Unreal Header Tool）相关的声明。如果你在结构体上加了 USTRUCT() 宏，就必须包含这个头文件。

UENUM(BlueprintType)  //这是一个 UENUM 宏，表示下面定义的枚举（enum）要暴露给 Unreal 的反射系统和蓝图。
                      //BlueprintType：允许在蓝图中使用这个枚举类型。
enum class EAvatarRequestMode : uint8  //定义一个枚举类型，名字叫 EAvatarRequestMode，底层用 uint8（一个字节）存储
{
    Audio UMETA(DisplayName="Audio"),  //音频文件输入模式。UMETA(DisplayName="Audio") 让它在蓝图里显示为 “Audio”。
    Text UMETA(DisplayName="Text"),  //文本输入模式
    Mic UMETA(DisplayName="Mic")  //麦克风实时输入模式
};

USTRUCT(BlueprintType)  //声明这是一个结构体，并且可以在蓝图中使用
struct FAvatarFaceFrame
{
    GENERATED_BODY()  //Unreal 必需的宏，用来生成反射代码

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category="VirtualHuman")  //UPROPERTY(...)：声明属性，让它可以被蓝图读取和编辑
                                                                         //BlueprintReadOnly：蓝图只能读取这个值，不能修改
                                                                         //EditAnywhere：可以在编辑器详情面板中编辑（在结构体实例上）
                                                                         //Category="VirtualHuman"：在编辑器中归到 “VirtualHuman” 分类下
    TArray<FString> Names;  //一个字符串数组，存储面部表情控制器的名字（比如 "jawOpen", "mouthSmile" 等）。

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category="VirtualHuman")
    TArray<float> Weights;  //一个浮点数数组，与 Names 一一对应，存储每个控制器的权重（0~1 之间）。
};

USTRUCT(BlueprintType)
struct FAvatarBodyFrame
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category="VirtualHuman")
    TArray<FString> ChannelNames;  //通道名称数组（比如关节名称或控制通道）

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category="VirtualHuman")
    TArray<float> Values;  //对应的数值数组（如旋转角度、位移等）

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category="VirtualHuman")
    FString Format;  //描述数据的格式（例如 "axis_angle" 或 "euler"），告诉使用者如何解析 Values
};

USTRUCT(BlueprintType)
struct FAvatarRuntimeFrame
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category="VirtualHuman")
    int32 Seq = -1;  //序列号，用于标识第几次请求/响应。默认值为 -1 表示无效

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category="VirtualHuman")
    FString Status;  //服务器返回的状态字符串（如 "success", "processing"）

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category="VirtualHuman")
    FString Emotion;  //识别出的情感标签（如 "happy", "sad"）

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category="VirtualHuman")
    TArray<uint8> AudioWav;  //服务器返回的音频数据（如果有的话），以 WAV 格式的字节数组存储

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category="VirtualHuman")
    FAvatarFaceFrame Face;  //面部表情数据（FAvatarFaceFrame 类型）

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category="VirtualHuman")
    FAvatarBodyFrame Body;  //身体动作数据（FAvatarBodyFrame 类型）
};
