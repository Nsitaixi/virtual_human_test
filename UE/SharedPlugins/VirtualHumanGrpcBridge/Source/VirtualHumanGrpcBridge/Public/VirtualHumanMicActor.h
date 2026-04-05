//这个文件定义了一个 AVirtualHumanMicActor 类，它是一个 Actor，可以放置在场景中，负责：绑定键盘按键（F 键按下/松开控制麦克风，G 键弹出文本输入框）
//自动寻找 MetaHuman 角色身上的面部网格体、身体网格体和 Control Rig 组件。接收 UAvatarGrpcSubsystem 发来的驱动数据，并应用到对应的组件上

#pragma once  //防止这个头文件被多次包含（重复编译）

#include "CoreMinimal.h"  //包含 Unreal 引擎最核心的基础类型（FString、TArray、UObject 等）
#include "GameFramework/Actor.h"  //引入 AActor 类的定义。可以在自己的 C++ 类中继承 AActor，从而获得位置、旋转、缩放、生命周期管理、网络同步等核心功能
#include "AvatarGrpcTypes.h"  //包含我们之前定义的数据结构（FAvatarRuntimeFrame、FAvatarFaceFrame、FAvatarBodyFrame 等）。这个 Actor 需要用到这些类型来存储和传递数据
#include "VirtualHumanMicActor.generated.h"  //这是 Unreal Header Tool (UHT) 自动生成的头文件，必须放在所有 UCLASS、USTRUCT、UENUM 等宏之后、类定义之前。它包含了反射相关的声明

class APlayerController;  //玩家控制器，用于绑定输入
class UAvatarGrpcSubsystem;  //我们之前写的子系统
class UControlRigComponent;  //Control Rig 组件，用于驱动身体动画
class USkeletalMeshComponent;  //骨骼网格体组件，用于面部 morph targets

UCLASS()  //告诉 Unreal 的反射系统这是一个 UObject 派生类，可以被蓝图继承、可以在编辑器中放置等。
class VIRTUALHUMANGRPCBRIDGE_API AVirtualHumanMicActor : public AActor  //VIRTUALHUMANGRPCBRIDGE_API：导出宏，使得这个类可以被其他模块使用
                                                                        //AVirtualHumanMicActor：类名，A 前缀表示继承自 AActor（可以放在场景中）
                                                                        //: public AActor：公开继承自 AActor，因此它拥有 Actor 的基本功能（位置、旋转、Tick 等）
{
	GENERATED_BODY()  //必需的宏，UHT 会在这里插入一些自动生成的函数声明（如 StaticClass()、GetLifetimeReplicatedProps() 等）

public:
	AVirtualHumanMicActor();  //声明构造函数。实现通常在 .cpp 文件中

protected:
	virtual void BeginPlay() override;  //virtual：允许子类重写
	                                    //BeginPlay：Actor 开始运行时调用（类似初始化）
	                                    //override：明确表示重写父类的虚函数，如果父类没有对应函数会编译报错
	virtual void Tick(float DeltaSeconds) override;  //Tick：每一帧调用，参数是帧间隔时间（秒）
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;  //EndPlay：Actor 销毁前调用，用于清理资源

private:
	UPROPERTY(EditInstanceOnly, Category = "VirtualHuman")  //EditInstanceOnly：这个属性只能在场景中的 Actor 实例上编辑，不能在蓝图类默认值中编辑
	                                                        //Category = "VirtualHuman"：在编辑器细节面板中归类到 “VirtualHuman” 分组下
	TObjectPtr<USkeletalMeshComponent> FaceMeshOverride;  //TObjectPtr<...>：Unreal 的智能指针，用于 UObject 派生类，自动处理引用计数
	                                                      //FaceMeshOverride：直接指定面部网格体组件（如果不指定，程序会自动查找）

	UPROPERTY(EditInstanceOnly, Category = "VirtualHuman")
	TObjectPtr<USkeletalMeshComponent> BodyMeshOverride;  //BodyMeshOverride：直接指定身体网格体组件

	UPROPERTY(EditInstanceOnly, Category = "VirtualHuman")
	TObjectPtr<AActor> MetaHumanActor;  //MetaHumanActor：指定 MetaHuman 角色的 Actor（通常是一个蓝图实例，比如 BP_Bes）

	UPROPERTY(EditInstanceOnly, Category = "VirtualHuman")
	TObjectPtr<UControlRigComponent> BodyControlRigOverride;  //BodyControlRigOverride：直接指定 Control Rig 组件

	UPROPERTY()  //这些属性没有 EditAnywhere 等修饰符，因此不能在编辑器中手动赋值，而是在运行时由代码自动填充
	TObjectPtr<UAvatarGrpcSubsystem> GrpcSubsystem;  //GrpcSubsystem：指向 UAvatarGrpcSubsystem 的指针，在 BeginPlay 中获取

	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> ResolvedFaceMesh;  //ResolvedFaceMesh：解析出来的面部网格体（可能是手动指定的，也可能是自动找到的）

	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> ResolvedBodyMesh;  //ResolvedBodyMesh：解析出来的身体网格体

	UPROPERTY()
	TObjectPtr<UControlRigComponent> ResolvedBodyControlRig;  //ResolvedBodyControlRig：解析出来的 Control Rig 组件

	FAvatarRuntimeFrame LatestFrame;  //LatestFrame：存储从 Subsystem 接收到的最新驱动数据帧
	bool bHasLatestFrame = false;  //bHasLatestFrame：标记是否已经收到过有效数据（用于 Tick 中判断是否应用）

	FTransform InitialBodyMeshTransform = FTransform::Identity;  //InitialBodyMeshTransform：身体网格体的初始相对变换（位置/旋转/缩放）。在“备用”身体驱动模式（没有 Control Rig）时，基于初始位置叠加偏移旋转，避免身体复位到零
	bool bCapturedInitialBodyMeshTransform = false;  //bCapturedInitialBodyMeshTransform：是否已经记录了初始变换

	UFUNCTION()  //UFUNCTION()：暴露给反射系统，但不加 BlueprintCallable，通常用于绑定委托
	void HandleFrameUpdated(FAvatarRuntimeFrame Frame);  //这个函数会绑定到 UAvatarGrpcSubsystem::OnFrameUpdated 事件上，当新帧到达时自动调用
	                                                     //参数 Frame 是接收到的新数据帧

	void OnMicPressed();  //F 键按下时调用，启动麦克风实时驱动
	void OnMicReleased();  //F 键松开时调用，停止麦克风
	void OnTextPressed();  //G 键按下时调用，弹出文本输入对话框，发送文本

	bool ShowTextInputDialog(FString& OutText) const;  //显示一个模态对话框，让用户输入文本
	                                                   //参数 OutText 输出用户输入的文本
	                                                   //返回值：true 表示用户点击了 OK 且有非空文本，false 表示取消或空文本

	void ResolveDriveTargets();  //根据手动指定的 Override 或自动查找，填充 ResolvedFaceMesh、ResolvedBodyMesh、ResolvedBodyControlRig
	void BindInput();  //绑定 F 键和 G 键的输入事件
	void BindSubsystemDelegate();  //将 HandleFrameUpdated 绑定到子系统的 OnFrameUpdated 事件上
	void UnbindSubsystemDelegate();  //解绑事件
	void ApplyLatestFrame();  //在 Tick 中调用，将 LatestFrame 的数据应用到面部和身体组件上
	void ApplyBodyFallbackMotion(const FAvatarBodyFrame& BodyFrame);  //当没有 Control Rig 时，用一个简单的备用方式驱动身体（仅旋转网格体，不处理骨骼）
};
