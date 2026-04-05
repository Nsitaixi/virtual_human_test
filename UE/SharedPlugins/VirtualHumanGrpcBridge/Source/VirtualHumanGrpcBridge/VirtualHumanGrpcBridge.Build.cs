//这个文件是 Unreal Engine 插件的“编译说明书”，它告诉 Unreal 的构建工具（UnrealBuildTool，UBT）如何编译你的 C++ 代码：需要哪些头文件、链接哪些库、开启什么编译选项、依赖哪些引擎模块等

using UnrealBuildTool;  //导入 Unreal 构建工具所需要的命名空间。告诉 C# 编译器，我们要使用 Unreal 自己定义的一些规则和类型（比如 ModuleRules 这个基类）
using System;  //导入 C# 基础系统功能。让我们能用 string、int 等基础类型，以及后面会用到的 Environment 等工具。
using System.IO;  //导入文件与目录操作的功能。让我们可以检查文件是否存在、拼接路径等。后面读取环境变量、找 .lib 文件都会用到。

public class VirtualHumanGrpcBridge : ModuleRules  //声明一个公共类，类名是 VirtualHumanGrpcBridge，它继承自 ModuleRules。这里定义了我们这个模块的“编译规则”。类名必须和这个 .Build.cs 的文件名一模一样。
{
    public VirtualHumanGrpcBridge(ReadOnlyTargetRules Target) : base(Target)  //构造函数，参数 Target 包含当前编译的目标信息（比如是编辑器还是游戏，是 Win64 还是 Android）。当 Unreal 编译这个模块时，会先调用这个函数来设置各种选项。
                                                                              //base(Target):调用父类的构造，让父类先处理一些基础工作
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;  //设置预编译头（PCH）的使用方式。预编译头是一种加速编译的技术。UseExplicitOrSharedPCHs 表示“使用显式或共享的预编译头”，这是 Unreal 推荐的现代模式。
        bEnableExceptions = true;  //启用 C++ 异常（try/catch）。gRPC 和 Protobuf 的代码内部使用了 C++ 异常，所以必须设置为 true，否则编译会报错。
        bUseRTTI = true;  //启用运行时类型识别（RTTI）。gRPC 也需要 RTTI，所以必须开启。RTTI 能让程序在运行时知道一个对象是什么类型。
        CppStandard = CppStandardVersion.Cpp20;  //指定使用 C++20 标准。

        PublicDependencyModuleNames.AddRange(new string[]  //开始添加公共依赖模块列表。
        {
            "Core",  //Unreal 的核心库（基本类型、容器、内存管理）。几乎所有模块都要依赖它。
            "CoreUObject",  //UObject 系统的支持（反射、序列化等）。
            "Engine",  //引擎级别的功能（Actor、Component、World 等）
            "InputCore",  //输入处理（键盘、鼠标）。我们的插件里用了 InputComponent->BindKey，所以需要它。
            "Voice",  //语音捕获功能。麦克风实时采集需要这个模块。
            "AudioMixer",  //音频混合器（虽然没直接用，但 Voice 可能间接依赖）
            "Slate",  //UI 框架。我们的插件弹出了文本输入对话框（SButton, SEditableTextBox），所以需要
            "SlateCore",  //同上
            "ApplicationCore",  //应用程序基础（窗口、消息循环）
            "ControlRig",  //控制绑定系统，用于驱动 MetaHuman 的身体和面部。
            "RigVM"  //ControlRig 底层的虚拟机
        });

        PrivateDependencyModuleNames.AddRange(new string[]  //开始添加私有依赖模块列表
        {
            "Projects"  //允许查询当前项目中的插件和模块信息
        });

        string GrpcRoot = Environment.GetEnvironmentVariable("GRPC_SDK_ROOT");  //读取环境变量 GRPC_SDK_ROOT 的值，存到字符串 GrpcRoot。需要提前在电脑上设置一个环境变量，告诉编译脚本gRPC SDK 安装在哪里。
        if (string.IsNullOrWhiteSpace(GrpcRoot))  //如果环境变量是空的（用户没设置）
        {
            GrpcRoot = @"D:\Nsitaixi\Unreal\gRPC\install";  //提供一个默认路径（硬编码）
        }

        string GrpcIncludeDir = Path.Combine(GrpcRoot, "include");  //拼接出 gRPC 的头文件目录。告诉编译器去哪里找 grpcpp/grpcpp.h 等头文件。
        string GrpcLibDir = Path.Combine(GrpcRoot, "lib");  //拼接出 gRPC 的库文件目录。告诉编译器去哪里找.lib文件所在位置

        string ProtoRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../ThirdParty/GrpcProto"));  //找到自定义的 Protobuf/gRPC 生成的静态库的位置。这个路径里面应该放你用 protoc 和 grpc_cpp_plugin 生成的 avatar_stream.pb.h/cc 以及编译好的静态库。
                                                                                                           //ModuleDirectory 是一个变量，由 Unreal 自动提供，表示当前 .Build.cs 文件所在的目录
                                                                                                           //Path.Combine(ModuleDirectory, "../../ThirdParty/GrpcProto")：从当前模块目录向上两级，然后进入 ThirdParty/GrpcProto
                                                                                                           //Path.GetFullPath(...)：将相对路径转为绝对路径
        string ProtoIncludeDir = Path.Combine(ProtoRoot, "include");  //拼接出 Protobuf 生成的头文件目录。里面应该有 avatar_stream.pb.h 和 avatar_stream.grpc.pb.h
        string ProtoLib = Path.Combine(ProtoRoot, "build", "Release", "VirtualHumanGrpcProto.lib");  //拼接出我们自己的静态库文件的完整路径。这个 .lib 文件是你之前用 CMake 编译出来的 VirtualHumanGrpcProto.lib（里面包含 protobuf 序列化代码和 gRPC stub 代码）。

        PublicSystemIncludePaths.Add(GrpcIncludeDir);  //把 gRPC 的头文件目录添加到系统包含路径。编译器会在这些目录里搜索 #include <grpcpp/grpcpp.h> 等头文件。PublicSystemIncludePaths 里的目录会被视为“系统”目录，不会产生警告。
        PublicSystemIncludePaths.Add(ProtoIncludeDir);  //把自定义 protobuf 头文件目录也加到系统包含路径。这样就能 #include "avatar_stream.pb.h" 了。

        if (!File.Exists(ProtoLib))  //检查我们需要的静态库文件是否存在。如果找不到 VirtualHumanGrpcProto.lib，说明之前编译没做好。
        {
            throw new BuildException("找不到静态库: " + ProtoLib);  //抛出一个构建异常，UBT 会停止编译并显示错误信息。
        }
        PublicAdditionalLibraries.Add(ProtoLib);  //告诉链接器去链接这个静态库。把 VirtualHumanGrpcProto.lib 里的代码合并到你的 Unreal 模块中。

        if (Directory.Exists(GrpcLibDir))  //如果 gRPC 的库目录存在
        {
            foreach (string Lib in Directory.GetFiles(GrpcLibDir, "*.lib"))  //遍历 GrpcLibDir 目录下所有 .lib 文件
            {
                string Name = Path.GetFileName(Lib);
                if (Name.Equals("libprotobuf.lib", StringComparison.OrdinalIgnoreCase))  //如果文件名是 libprotobuf.lib，就跳过（不添加到链接列表）。libprotobuf.lib 是 protobuf 的库，我们后面会单独添加它，避免重复或顺序问题。
                {
                    continue;
                }

                PublicAdditionalLibraries.Add(Lib);  //把这个 .lib 文件添加到链接列表。这些是 gRPC 所需的库，比如 grpc++.lib、gpr.lib 等
            }
        }

        string ProtobufLib = Path.Combine(GrpcLibDir, "libprotobuf.lib");  //构造 protobuf 库的完整路径
        if (File.Exists(ProtobufLib))  //检查这个文件是否存在
        {
            PublicAdditionalLibraries.Add(ProtobufLib);  //链接 protobuf 库。protobuf 的序列化/反序列化代码需要这个库
        }
        else  //如果找不到 libprotobuf.lib，就报错。
        {
            throw new BuildException("缺少 libprotobuf.lib，请检查 gRPC 安装是否完整: " + ProtobufLib);
        }

        if (Target.Platform == UnrealTargetPlatform.Win64)  //如果当前编译的目标平台是 64 位 Windows。下面的链接库只在 Windows 上需要，Linux 或 Mac 不需要这些系统库。
        {
            PublicSystemLibraries.AddRange(new string[]  //添加 Windows 系统需要的额外链接库。
            {
                "ws2_32.lib",  //Windows 的网络
                "bcrypt.lib",  //加密
                "crypt32.lib",  //加密
                "secur32.lib",  //加密
                "iphlpapi.lib",  //网络接口
                "advapi32.lib",  //系统 API
                "dbghelp.lib",  //调试
                "shlwapi.lib"  //外壳
            });
        }
    }
}