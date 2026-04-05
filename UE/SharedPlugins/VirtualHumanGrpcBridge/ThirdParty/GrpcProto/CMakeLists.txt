cmake_minimum_required(VERSION 3.24)  //设置 CMake 的最低版本要求
project(VirtualHumanGrpcProto LANGUAGES CXX)  //定义项目的名称和使用的编程语言

set(CMAKE_CXX_STANDARD 20)  //设置 C++ 语言标准为 C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)  //要求编译器必须支持 C++20，不能降级

find_package(Protobuf CONFIG REQUIRED)  //查找系统中已安装的 Protobuf 库（Google Protocol Buffers）
                                        //find_package 是 CMake 提供的命令，用来找第三方库
                                        //Protobuf 是库的名字
                                        //CONFIG 表示使用库自己提供的配置文件（protobuf-config.cmake），而不是 CMake 内置的旧查找模块
                                        //REQUIRED 表示这个库是必须的，如果找不到，CMake 会直接报错并停止
find_package(gRPC CONFIG REQUIRED)  //查找 gRPC 库。我们需要 gRPC 来做网络通信（客户端和服务端之间的流式传输）

add_library(VirtualHumanGrpcProto STATIC  //创建一个库目标，名字叫 VirtualHumanGrpcProto，类型是 静态库（STATIC）
    src/avatar_stream.pb.cc  //这个文件是由 protoc 编译器从 avatar_stream.proto 生成的，里面包含了 protobuf 消息的序列化/反序列化代码（比如 StreamRequest、StreamResponse 的读写方法）
    src/avatar_stream.grpc.pb.cc  //这也是由 protoc 配合 grpc_cpp_plugin 生成的，里面包含了 gRPC 服务的客户端 stub 和服务器 skeleton 代码（比如 AvatarService::Stub 的实现）
)

target_include_directories(VirtualHumanGrpcProto  //为这个库目标设置头文件搜索路径。告诉编译器去哪里找 .h 头文件（比如 avatar_stream.pb.h）
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include  //CMAKE_CURRENT_SOURCE_DIR 是 CMake 内置变量，表示当前正在处理的 CMakeLists.txt 所在的目录
)

target_link_libraries(VirtualHumanGrpcProto  //为这个库目标指定要链接的其他库。我们编译出的静态库 VirtualHumanGrpcProto.lib 内部使用了 protobuf 和 gRPC 的函数，所以必须把 protobuf 和 gRPC 的库也链接进来。
    PRIVATE
        protobuf::libprotobuf  //引用 protobuf 库（由 find_package(Protobuf) 导入的目标）。这个代表 libprotobuf.lib 或 libprotobuf.a，包含了 protobuf 的运行时代码。
        gRPC::grpc++  //引用 gRPC 的 C++ 核心库。这个代表 grpc++.lib 或 libgrpc++.a，包含了 gRPC 客户端和服务器的实现。
)