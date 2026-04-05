//irtualHumanGrpcBridge.h 和 .cpp 是模块入口文件，告诉 Unreal 这个插件存在并可以加载

#include "VirtualHumanGrpcBridge.h"  //包含对应的头文件
#include "Modules/ModuleManager.h"  //包含模块管理器的头文件。模块管理器是 Unreal 用来加载、卸载模块的系统。我们需要用它来注册我们的模块

IMPLEMENT_MODULE(FDefaultModuleImpl, VirtualHumanGrpcBridge)  //这是一个 Unreal 规定的宏（macro），用来声明模块入口。整个插件就是通过这个宏被 Unreal 识别和加载的。
                                                              //FDefaultModuleImpl：模块的默认实现类（一个空的、最简单的模块类，只提供基础功能）
                                                              //VirtualHumanGrpcBridge：模块的名称（必须和 .Build.cs 里的模块名、.uplugin 里的模块名一致）
