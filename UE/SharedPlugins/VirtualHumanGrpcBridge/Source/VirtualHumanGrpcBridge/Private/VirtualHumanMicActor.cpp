#include "VirtualHumanMicActor.h"  //包含对应的头文件，这样编译器就知道类的声明

#include "AvatarGrpcSubsystem.h"  //包含 gRPC 子系统头文件，因为我们要调用它的方法（如 StartMicRealtime、RunText 等）
#include "Components/SkeletalMeshComponent.h"  //包含骨骼网格体组件，用于操作面部和身体网格。
#include "ControlRigComponent.h"  //包含 Control Rig 组件，用于驱动身体动画
#include "Engine/GameInstance.h"  //包含游戏实例类，用来获取子系统
#include "Engine/World.h"  //包含世界类，通常用于获取游戏实例等
#include "Framework/Application/SlateApplication.h"  //包含 Slate UI 框架的应用程序类，用于创建模态对话框
#include "GameFramework/PlayerController.h"  //包含玩家控制器，用于绑定输入
#include "InputCoreTypes.h"  //包含输入核心类型（如 EKeys::F、IE_Pressed 等）
#include "Kismet/GameplayStatics.h"  //包含游戏静态函数库，例如 GetPlayerController
#include "Widgets/Input/SButton.h"  //Slate UI 的各种控件，用于构建文本输入对话框：按钮、文本框、边框、布局盒子、窗口、文本块等
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

AVirtualHumanMicActor::AVirtualHumanMicActor()
{
	PrimaryActorTick.bCanEverTick = true;  //PrimaryActorTick 是 AActor 的一个结构体成员，控制 Actor 的 Tick 行为
	                                       //bCanEverTick = true 表示这个 Actor 需要每帧调用 Tick 函数
	AutoReceiveInput = EAutoReceiveInput::Player0;  //自动接收玩家 0 的输入，这样按下键盘时才会触发绑定的回调
}

void AVirtualHumanMicActor::BeginPlay()
{
	Super::BeginPlay();  //调用父类（AActor）的 BeginPlay，确保父类的初始化逻辑执行

	if (UGameInstance* GameInstance = GetGameInstance())  //GetGameInstance() 获取当前运行的游戏实例对象。如果成功，将其存入局部变量 GameInstance
	{
		GrpcSubsystem = GameInstance->GetSubsystem<UAvatarGrpcSubsystem>();  //从游戏实例中获取 UAvatarGrpcSubsystem 子系统的指针，存入成员变量 GrpcSubsystem
	}

	ResolveDriveTargets();  //解析驱动目标：根据手动指定的组件或自动查找，填充 ResolvedFaceMesh、ResolvedBodyMesh、ResolvedBodyControlRig
	BindSubsystemDelegate();  //将 HandleFrameUpdated 函数绑定到子系统的 OnFrameUpdated 事件上，这样当新数据到达时，这个 Actor 会自动收到通知
	BindInput();  //绑定键盘输入（F 键和 G 键）

	UE_LOG(LogTemp, Log, TEXT("VirtualHumanMicActor BeginPlay OK. Subsystem=%s FaceMesh=%s BodyMesh=%s BodyRig=%s"),  //输出日志，显示子系统是否有效、解析到的面部网格、身体网格和 Control Rig 的名字（或 "Null"）
		GrpcSubsystem ? TEXT("Valid") : TEXT("Null"),
		ResolvedFaceMesh ? *ResolvedFaceMesh->GetName() : TEXT("Null"),
		ResolvedBodyMesh ? *ResolvedBodyMesh->GetName() : TEXT("Null"),
		ResolvedBodyControlRig ? *ResolvedBodyControlRig->GetName() : TEXT("Null"));
}

void AVirtualHumanMicActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);  //调用父类的 Tick，确保父类的每帧逻辑执行
	ApplyLatestFrame();  //将最新收到的驱动数据（面部、身体）应用到对应的组件上
}

void AVirtualHumanMicActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindSubsystemDelegate();  //解绑之前绑定的事件，避免 Actor 销毁后子系统还尝试调用已经失效的回调

	if (GrpcSubsystem && GrpcSubsystem->IsMicRealtimeRunning())  //如果子系统存在且麦克风正在运行，则调用 StopMicRealtime() 停止麦克风录制
	{
		GrpcSubsystem->StopMicRealtime();
	}

	Super::EndPlay(EndPlayReason);  //调用父类的清理逻辑
}

void AVirtualHumanMicActor::BindInput()
{
	APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);  //获取玩家控制器（玩家 0，即第一个玩家）
	if (!PlayerController)  //如果获取失败，打印警告并返回
	{
		UE_LOG(LogTemp, Warning, TEXT("VirtualHumanMicActor: PlayerController is null, input binding skipped."));
		return;
	}

	EnableInput(PlayerController);  //为这个 Actor 启用输入，这样它才能接收键盘事件

	if (!InputComponent)  //InputComponent 是 AActor 的一个成员，在 EnableInput 后会自动创建。如果仍然为空，说明出错了，打印错误并返回
	{
		UE_LOG(LogTemp, Error, TEXT("VirtualHumanMicActor: InputComponent is null after EnableInput."));
		return;
	}

	InputComponent->BindKey(EKeys::F, IE_Pressed, this, &AVirtualHumanMicActor::OnMicPressed);  //EKeys::F 键，按下（IE_Pressed）时调用 OnMicPressed
	InputComponent->BindKey(EKeys::F, IE_Released, this, &AVirtualHumanMicActor::OnMicReleased);  //EKeys::F 键，释放（IE_Released）时调用 OnMicReleased
	InputComponent->BindKey(EKeys::G, IE_Pressed, this, &AVirtualHumanMicActor::OnTextPressed);  //EKeys::G 键，按下时调用 OnTextPressed

	UE_LOG(LogTemp, Log, TEXT("VirtualHumanMicActor: F/G key bindings installed."));  //最后打印日志表示绑定成功
}

void AVirtualHumanMicActor::BindSubsystemDelegate()
{
	if (!GrpcSubsystem)  //检查 GrpcSubsystem 是否有效，无效则警告并返回
	{
		UE_LOG(LogTemp, Warning, TEXT("VirtualHumanMicActor: GrpcSubsystem is null, delegate binding skipped."));
		return;
	}

	GrpcSubsystem->OnFrameUpdated.AddUniqueDynamic(this, &AVirtualHumanMicActor::HandleFrameUpdated);  //OnFrameUpdated 是子系统中的动态多播委托
	                                                                                                   //AddUniqueDynamic 将当前 Actor 的 HandleFrameUpdated 函数添加到委托的调用列表中，并且不会重复添加同一个函数
}

void AVirtualHumanMicActor::UnbindSubsystemDelegate()
{
	if (!GrpcSubsystem)  //检查子系统是否有效，无效则直接返回
	{
		return;
	}

	GrpcSubsystem->OnFrameUpdated.RemoveDynamic(this, &AVirtualHumanMicActor::HandleFrameUpdated);  //从委托的调用列表中移除 HandleFrameUpdated 函数。把之前注册的事件回调取消掉，避免在 Actor 销毁后子系统还试图调用一个已经不存在的函数
}

bool AVirtualHumanMicActor::ShowTextInputDialog(FString& OutText) const  //参数 OutText：输出参数，用于返回用户输入的文本（引用传递，函数内部修改会影响到外部变量）
                                                                         //const：表示这个函数不会修改 Actor 的成员变量（除了输出参数）
                                                                         //返回值：true 表示用户点击了 OK 并且输入了非空文本，false 表示取消或空文本
{
	OutText.Reset();  //清空输出字符串，确保之前的内容被清除

	if (!FSlateApplication::IsInitialized())  //如果没有初始化（例如在专用服务器或某些命令行模式），则无法创建对话框，直接返回 false
	                                          //FSlateApplication 是 Unreal 的 UI 框架（Slate）的应用程序单例
	                                          //IsInitialized() 返回 Slate 是否已经初始化（通常在游戏启动时已经初始化）
	{
		return false;
	}

	TSharedPtr<SEditableTextBox> InputBox;  //智能指针，指向一个 SEditableTextBox（可编辑文本框控件），用于让用户输入文本
	TSharedPtr<SWindow> Window;  //智能指针，指向一个 SWindow（窗口控件），对话框本身就是一个窗口

	Window = SNew(SWindow)  //使用了 Unreal 的 Slate 声明式语法，类似 C++ 的“流式”构建 UI
	                        //SNew(SWindow)：创建一个新的窗口控件，并将智能指针 Window 指向它
		.Title(FText::FromString(TEXT("输入要发送给 LLM 的文本")))  //.Title(...)：设置窗口标题，使用 FText::FromString 将字符串转换为 FText
		.ClientSize(FVector2D(620.f, 180.f))  //.ClientSize(...)：设置窗口内容区域的大小（宽度 620，高度 180 像素）
		.SizingRule(ESizingRule::Autosized)  //窗口大小自动适应内容（实际上 ClientSize 和 Autosized 同时使用可能会冲突，但这里主要是给一个初始大小）
		.SupportsMinimize(false)  //禁止窗口最小化
		.SupportsMaximize(false)  //禁止窗口最大化
		[
			SNew(SBorder)  //创建一个带边框的容器
			.Padding(16)  //内边距 16 像素，使内容离边框有一定距离
			[  //方括号 [...] 表示该控件的子内容
				SNew(SVerticalBox)  //SVerticalBox：垂直排列子控件

				+ SVerticalBox::Slot()  //添加一个垂直槽位
				.AutoHeight()  //槽位高度根据内容自动调整
				.Padding(0, 0, 0, 10)  //设置内边距（左，上，右，下），这里底部留 10 像素
				[
					SNew(STextBlock)  //第一个槽位：一个文本块 STextBlock，显示提示文字
					.Text(FText::FromString(TEXT("请输入一句话，然后点击 OK：")))
				]

				+ SVerticalBox::Slot()  //第二个槽位
				.AutoHeight()
				.Padding(0, 0, 0, 12)
				[
					SAssignNew(InputBox, SEditableTextBox)  //创建一个可编辑文本框，并将智能指针 InputBox 指向它
					.MinDesiredWidth(560.f)  //最小宽度 560 像素
				]

				+ SVerticalBox::Slot()  //第三个槽位：一个水平盒子 SHorizontalBox，里面放两个按钮
				.AutoHeight()
				[
					SNew(SHorizontalBox)  //SHorizontalBox：水平排列子控件

					+ SHorizontalBox::Slot()  //+ SHorizontalBox::Slot()：添加一个水平槽位
					.AutoWidth()  //宽度根据内容自动调整
					.Padding(0, 0, 10, 0)  //右边留 10 像素间距
					[
						SNew(SButton)  //创建一个按钮
						.Text(FText::FromString(TEXT("OK")))  //.Text(...)：按钮上的文字
						.OnClicked_Lambda([&OutText, InputBox, Window]()  //.OnClicked_Lambda(...)：按钮点击时执行的 Lambda 表达式（匿名函数）
						                                                  //&OutText（按引用捕获外部输出字符串），InputBox 和 Window（按值捕获智能指针）
						{
							if (InputBox.IsValid())  //检查 InputBox 是否有效，如果有效，获取文本框中的文本（GetText() 返回 FText，再 .ToString() 转为 FString），赋值给 OutText
							{
								OutText = InputBox->GetText().ToString();
							}

							if (Window.IsValid())  //检查 Window 是否有效，如果有效，调用 RequestDestroyWindow() 关闭窗口
							{
								Window->RequestDestroyWindow();
							}

							return FReply::Handled();  //表示事件已被处理，不再传递
						})
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Cancel")))
						.OnClicked_Lambda([&OutText, Window]()
						{
							OutText.Reset();  //直接清空 OutText（表示用户取消或未输入）

							if (Window.IsValid())  //关闭窗口
							{
								Window->RequestDestroyWindow();
							}

							return FReply::Handled();
						})
					]
				]
			]
		];

	FSlateApplication::Get().AddModalWindow(Window.ToSharedRef(), nullptr);  //FSlateApplication::Get()：获取 Slate 应用程序单例
	                                                                         //.AddModalWindow(Window.ToSharedRef(), nullptr)：将 Window 作为模态窗口显示。第二个参数是父窗口，这里 nullptr 表示没有父窗口。模态窗口会阻塞后续代码的执行，直到窗口关闭。
	return !OutText.IsEmpty();  //如果 OutText 非空（用户输入了文本并点击 OK），返回 true；否则返回 false（取消或空文本）
}

void AVirtualHumanMicActor::OnMicPressed()  //F 键按下时启动麦克风（因为在 BindInput 中绑定了）
{
	if (!GrpcSubsystem)  //检查子系统的指针是否有效（! 表示“不是”）。如果 GrpcSubsystem 为空（例如没有正确获取到），则执行大括号内的代码
	{
		UE_LOG(LogTemp, Error, TEXT("VirtualHumanMicActor: GrpcSubsystem is null."));  //输出错误日志，提示子系统为空
		return;
	}

	ResolveDriveTargets();  //调用解析函数，重新确认或查找要驱动的面部网格、身体网格和 Control Rig 组件

	if (!GrpcSubsystem->IsMicRealtimeRunning())  //检查麦克风实时模式是否已经在运行
	{
		GrpcSubsystem->StartMicRealtime();  //调用子系统的 StartMicRealtime() 方法，启动后台麦克风采集和流式发送
		UE_LOG(LogTemp, Log, TEXT("VirtualHumanMicActor: Mic realtime started."));  //输出普通日志，表示麦克风已启动
	}
}

void AVirtualHumanMicActor::OnMicReleased()  // F 键松开时停止麦克风
{
	if (!GrpcSubsystem)  //同样先检查子系统是否有效
	{
		UE_LOG(LogTemp, Error, TEXT("VirtualHumanMicActor: GrpcSubsystem is null."));
		return;
	}

	if (GrpcSubsystem->IsMicRealtimeRunning())  //如果麦克风正在运行，则调用 StopMicRealtime() 停止它，并打印日志
	{
		GrpcSubsystem->StopMicRealtime();
		UE_LOG(LogTemp, Log, TEXT("VirtualHumanMicActor: Mic realtime stopped."));
	}
}

void AVirtualHumanMicActor::OnTextPressed()  //G 键按下时发送文本
{
	if (!GrpcSubsystem)
	{
		UE_LOG(LogTemp, Error, TEXT("VirtualHumanMicActor: GrpcSubsystem is null."));
		return;
	}

	FString Text;  //定义一个空的字符串变量，用于存储用户输入的文本
	if (!ShowTextInputDialog(Text))  //调用之前解释过的 ShowTextInputDialog 函数，它会显示一个对话框，让用户输入文本，并通过 Text 参数返回。如果函数返回 false（用户取消或没有输入文本），则进入内部
	{
		UE_LOG(LogTemp, Log, TEXT("VirtualHumanMicActor: text input cancelled or empty."));  //输出一条普通日志，表示输入被取消或为空
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("VirtualHumanMicActor: LLM prompt submitted: %s"), *Text);  //打印日志（显示提交的内容）
	GrpcSubsystem->RunText(Text);  //将文本发送给 gRPC 服务器
}

void AVirtualHumanMicActor::ResolveDriveTargets()  //查找要驱动的组件
{
	ResolvedFaceMesh = nullptr;  //先将三个解析出的指针置为 nullptr（清空）
	ResolvedBodyMesh = nullptr;
	ResolvedBodyControlRig = nullptr;

	if (FaceMeshOverride)  //如果用户手动指定了 FaceMeshOverride（在编辑器里拖拽赋值了面部网格体组件），就直接用它，不再自动查找
	{
		ResolvedFaceMesh = FaceMeshOverride;
	}

	if (BodyMeshOverride)  //同理处理 BodyMeshOverride 和 BodyControlRigOverride
	{
		ResolvedBodyMesh = BodyMeshOverride;
	}

	if (BodyControlRigOverride)
	{
		ResolvedBodyControlRig = BodyControlRigOverride;
	}

	if (!MetaHumanActor)  //如果没有指定 MetaHumanActor（要驱动的 MetaHuman 角色），则输出警告并直接返回
	{
		UE_LOG(LogTemp, Warning, TEXT("VirtualHumanMicActor: MetaHumanActor is null, please assign BP_Bes in the Details panel."));
		return;
	}

	TArray<USkeletalMeshComponent*> SkeletalMeshes;  //创建一个数组，用于存放 MetaHumanActor 身上所有的骨骼网格体组件
	MetaHumanActor->GetComponents<USkeletalMeshComponent>(SkeletalMeshes);  //获取该 Actor 下的所有 USkeletalMeshComponent，存入数组

	for (USkeletalMeshComponent* Comp : SkeletalMeshes)  // 遍历每个组件
	{  //遍历 MetaHuman Actor 身上所有的网格组件，通过名字和模型名字猜测哪个是脸、哪个是身体。找到后记录下来，并记住身体网格的初始位置，以便后面做简单的旋转驱动
		if (!Comp)  //跳过空指针
		{
			continue;
		}

		const FString CompName = Comp->GetName();  //获取组件的名字（例如 "FaceMesh"）
		const FString MeshName = Comp->GetSkeletalMeshAsset() ? Comp->GetSkeletalMeshAsset()->GetName() : FString();  //如果组件有骨骼网格资产（即实际模型），则获取模型的名字；否则设为空字符串

		const bool bLooksLikeFace =  //判断这个组件“看起来像面部网格”。只要满足任意一个条件，就认为是面部
			CompName.Contains(TEXT("face"), ESearchCase::IgnoreCase) ||  //组件名字包含 "face"（不区分大小写）
			MeshName.Contains(TEXT("face"), ESearchCase::IgnoreCase) ||  //或者模型名字包含 "face"
			MeshName.Contains(TEXT("Bes_FaceMesh"), ESearchCase::IgnoreCase);  //或者模型名字包含 "Bes_FaceMesh"（MetaHuman 面部的常见命名）

		const bool bLooksLikeBody =  //判断是否像身体网格
			CompName.Contains(TEXT("body"), ESearchCase::IgnoreCase) ||
			CompName.Contains(TEXT("torso"), ESearchCase::IgnoreCase) ||
			MeshName.Contains(TEXT("body"), ESearchCase::IgnoreCase) ||
			MeshName.Contains(TEXT("torso"), ESearchCase::IgnoreCase) ||
			MeshName.Contains(TEXT("m_med_unw"), ESearchCase::IgnoreCase);

		if (!ResolvedFaceMesh && bLooksLikeFace)  //如果还没有找到面部网格（!ResolvedFaceMesh）且当前组件符合面部特征，就把它赋给 ResolvedFaceMesh
		{
			ResolvedFaceMesh = Comp;
		}

		if (!ResolvedBodyMesh && bLooksLikeBody)  //如果还没有找到身体网格（!ResolvedBodyMesh）且当前组件符合身体特征，就把它赋给 ResolvedBodyMesh，并记录其初始相对变换（用于备用身体驱动）
		{
			ResolvedBodyMesh = Comp;
			InitialBodyMeshTransform = Comp->GetRelativeTransform();
			bCapturedInitialBodyMeshTransform = true;
		}
	}

	if (!ResolvedBodyControlRig)  //如果没有手动指定 Control Rig 组件，就在 MetaHumanActor 上查找 UControlRigComponent 类型的组件（通常只有一个）
	{
		ResolvedBodyControlRig = MetaHumanActor->FindComponentByClass<UControlRigComponent>();  //FindComponentByClass 返回第一个找到的该类型的组件
	}

	if (!ResolvedFaceMesh && SkeletalMeshes.Num() > 0)  //实在找不到哪个是脸哪个是身体，就随便拿第一个网格当脸，也当身体（虽然不科学，但至少程序不会崩溃）
	{
		ResolvedFaceMesh = SkeletalMeshes[0];
	}

	if (!ResolvedBodyMesh && SkeletalMeshes.Num() > 0)
	{
		ResolvedBodyMesh = SkeletalMeshes[0];
		if (!bCapturedInitialBodyMeshTransform)
		{
			InitialBodyMeshTransform = ResolvedBodyMesh->GetRelativeTransform();
			bCapturedInitialBodyMeshTransform = true;
		}
	}
}

void AVirtualHumanMicActor::HandleFrameUpdated(FAvatarRuntimeFrame Frame)  //这是一个成员函数，参数是 FAvatarRuntimeFrame（按值传递，拷贝一份）
                                                                           //这个函数被绑定到 UAvatarGrpcSubsystem::OnFrameUpdated 事件上，当子系统收到服务器的新数据时，会自动调用这个函数
{
	LatestFrame = MoveTemp(Frame);  //LatestFrame 是 Actor 的成员变量，用于存储最新一帧的驱动数据
	                                //MoveTemp(Frame) 是 Unreal 的移动语义（类似 std::move），将 Frame 的内容转移到 LatestFrame，避免深层拷贝（提高性能）。移动后，原来的 Frame 不再使用。
	bHasLatestFrame = true;  //设置标志位，表示已经收到过有效数据，方便 Tick 中判断是否需要应用
}

void AVirtualHumanMicActor::ApplyLatestFrame()  //每帧应用数据到虚拟人
{
	if (!bHasLatestFrame || !GrpcSubsystem)  //如果还没有收到过任何数据（bHasLatestFrame 为 false）或者子系统指针无效，就直接 return，不做任何事
	{
		return;
	}

	if (!ResolvedFaceMesh || !ResolvedBodyMesh)  //如果面部网格或身体网格还没有解析出来（例如在 BeginPlay 时 MetaHuman Actor 可能尚未完全初始化，或者后来发生了变化），则调用 ResolveDriveTargets() 重新查找
	{
		ResolveDriveTargets();
	}

	if (ResolvedFaceMesh && LatestFrame.Face.Names.Num() > 0 && LatestFrame.Face.Weights.Num() > 0)  //条件：面部网格有效，并且 LatestFrame.Face 中至少有一个表情控制点（名字和权重数组非空）
	{
		GrpcSubsystem->ApplyFaceFrameToMesh(ResolvedFaceMesh, LatestFrame.Face, 1.0f);  //调用子系统的 ApplyFaceFrameToMesh 函数，传入面部网格、面部数据、权重缩放系数（1.0 表示原始强度）。该函数内部会遍历所有 morph target，设置权重，让模型做表情
	}

	if (ResolvedBodyControlRig && LatestFrame.Body.Values.Num() > 0)  //如果找到了 ResolvedBodyControlRig 且身体数据数组非空
	{
		GrpcSubsystem->ApplyBodyFrameToControlRig(ResolvedBodyControlRig, LatestFrame.Body, 1.0f);  //调用子系统的 ApplyBodyFrameToControlRig，将身体动作数据应用到 Control Rig 上（实现精确的骨骼动画）
	}
	else if (ResolvedBodyMesh && LatestFrame.Body.Values.Num() > 0)  //如果没有 Control Rig，但身体网格存在且有数据
	{
		ApplyBodyFallbackMotion(LatestFrame.Body);  //调用 ApplyBodyFallbackMotion（一个简单的备用驱动，直接旋转整个网格体）
	}
}

void AVirtualHumanMicActor::ApplyBodyFallbackMotion(const FAvatarBodyFrame& BodyFrame)  //备用身体驱动（简单旋转）
                                                                                        //const FAvatarBodyFrame& BodyFrame：引用传递，避免拷贝
{  //如果没有 Control Rig（例如某些简易模型），我们就用一个最简单的办法：把服务器返回的前三个数值当作身体的旋转角度（Pitch, Yaw, Roll），直接旋转整个身体网格体，让它跟着语音或文字摆动。虽然不够精细，但至少能让用户感觉到虚拟人在“动”
	if (!ResolvedBodyMesh || BodyFrame.Values.Num() <= 0)  //如果身体网格无效或没有身体数据，直接返回
	{
		return;
	}

	if (!bCapturedInitialBodyMeshTransform)  //如果还没有记录身体的初始相对变换（位置、旋转、缩放），则获取当前值并保存。这个初始值将在每次旋转时作为基准，避免身体复位到零点
	{
		InitialBodyMeshTransform = ResolvedBodyMesh->GetRelativeTransform();
		bCapturedInitialBodyMeshTransform = true;
	}

	const float Pitch = BodyFrame.Values.IsValidIndex(0) ? BodyFrame.Values[0] * 20.0f : 0.0f;  //BodyFrame.Values 是一个浮点数数组。在备用模式中，我们假设前三个值分别代表 Pitch（俯仰）、Yaw（偏航）、Roll（滚转）
	                                                                                            //IsValidIndex(0) 检查索引 0 是否在数组范围内（防止越界）。如果在，取该值乘以 20.0 作为角度（放大系数，使动作更明显）；如果不在，则设为 0
	const float Yaw   = BodyFrame.Values.IsValidIndex(1) ? BodyFrame.Values[1] * 20.0f : 0.0f;
	const float Roll  = BodyFrame.Values.IsValidIndex(2) ? BodyFrame.Values[2] * 20.0f : 0.0f;

	const FRotator OffsetRot(Pitch, Yaw, Roll);  //FRotator OffsetRot(Pitch, Yaw, Roll); 创建一个旋转体（欧拉角）
	ResolvedBodyMesh->SetRelativeTransform(  //将身体的相对变换设置为此值。由于我们保持了初始位置和缩放，只改变了旋转，所以身体会在原地摆动，而不会位移或缩放
		FTransform(OffsetRot.Quaternion(), InitialBodyMeshTransform.GetLocation(), InitialBodyMeshTransform.GetScale3D()));  //OffsetRot.Quaternion() 将欧拉角转换为四元数（Unreal 内部使用）
		                                                                                                                     //创建一个新的 FTransform：旋转 = 当前计算出的四元数，平移 = 之前保存的初始位置，缩放 = 之前保存的初始缩放
}
