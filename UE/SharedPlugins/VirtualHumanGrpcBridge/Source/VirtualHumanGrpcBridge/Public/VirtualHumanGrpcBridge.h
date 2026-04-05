//irtualHumanGrpcBridge.h 和 .cpp 是模块入口文件，告诉 Unreal 这个插件存在并可以加载

#pragma once  //防止头文件被多次包含（multiple inclusion）。告诉编译器“这个文件只处理一次”，避免重复定义错误。

#include "CoreMinimal.h"  //包含 Unreal 引擎的核心基础头文件。这行会引入 Unreal 最常用的类型（如 FString、TArray、UObject 等），否则我们写的代码会缺少很多基本东西
