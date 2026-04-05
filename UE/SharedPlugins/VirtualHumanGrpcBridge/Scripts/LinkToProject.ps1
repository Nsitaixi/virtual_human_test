#使用方法：脚本所在目录下  .\LinkToProject.ps1 -ProjectRoot "D:\Nsitaixi\Unreal\UnrealProjects\新项目名"

param(  #PowerShell 中的关键字，意思是“参数”。它用来定义这个脚本接受哪些外部输入
    [string]$ProjectRoot = "D:\Nsitaixi\Unreal\UnrealProjects\virtual_human_test",  #[string]：数据类型，表示“字符串”（文本）。这里指定变量 $ProjectRoot 只能存放文本
                                                                                    #$ProjectRoot：变量名，用来存储“项目的根目录路径”。$ 是 PowerShell 中变量的标志
    [string]$SharedPluginRoot = "D:\Nsitaixi\Unreal\SharedPlugins\VirtualHumanGrpcBridge"  #同样定义了一个字符串变量 $SharedPluginRoot，默认值是共享插件的存放路径
)

$ErrorActionPreference = "Stop"  #$ErrorActionPreference：PowerShell 的一个内置变量，用来控制“当发生错误时，脚本的行为”
                                 #"Stop"：意思是“遇到错误就立即停止整个脚本，不再继续执行后面的命令”

$Dst = Join-Path $ProjectRoot "Plugins\VirtualHumanGrpcBridge"  #把项目根目录和后面的相对路径拼起来，得到类似 D:\...\virtual_human_test\Plugins\VirtualHumanGrpcBridge 的完整路径，并保存到 $Dst 变量中
                                                                #$Dst：变量名，Dst 是 Destination（目标路径）的缩写
                                                                #Join-Path：PowerShell 的一个命令，用来“拼接路径”。它会自动处理好反斜杠 \
                                                                #$ProjectRoot：之前定义的项目根目录（例如 D:\...\virtual_human_test）
                                                                #"Plugins\VirtualHumanGrpcBridge"：要拼接的文件夹名字

if (Test-Path $Dst) {  #Test-Path $Dst：Test-Path 是一个命令，用来检查某个路径是否存在。如果存在，返回 $true（真），否则返回 $false（假）
    Remove-Item $Dst -Recurse -Force  #Remove-Item：删除文件或文件夹的命令
                                      #$Dst：要删除的目标路径
                                      #-Recurse：参数（也叫“开关”），意思是“递归删除”——如果文件夹里面有子文件夹和文件，也一并删除
                                      #-Force：强制删除，即使文件是只读或隐藏的，也会删掉
}

New-Item -ItemType Junction -Path $Dst -Target $SharedPluginRoot | Out-Null  #这一行创建了一个“链接”，把项目的插件文件夹指向共享插件文件夹。项目以为插件就在自己目录里，实际上内容在别处。而且创建过程不显示任何信息
                                                                             #New-Item：创建新“东西”的命令，可以是文件、文件夹，或者特殊链接
                                                                             #-ItemType Junction：指定要创建的是一个“目录链接”（Junction）。你可以理解为“高级快捷方式”——在 Windows 中，它让一个文件夹看起来像真实存在，实际内容在另一个位置
                                                                             #-Path $Dst：新链接要放在哪里（即前面计算出的项目插件路径）
                                                                             #-Target $SharedPluginRoot：这个链接指向哪个真实文件夹（共享插件的位置）
                                                                             #|：管道符，意思是把前面命令的输出结果，传递给后面的命令
                                                                             #Out-Null：一个命令，作用是“丢弃任何输出”。因为 New-Item 创建完后会在屏幕上显示一些信息，但我们不想看到它，就用 Out-Null 把它扔掉
Write-Host "Linked $Dst -> $SharedPluginRoot"  #Write-Host：在控制台（命令行窗口）输出一行文字。告诉用户链接创建成功，以及源和目标分别是什么
                                               #"Linked $Dst -> $SharedPluginRoot"：一个字符串，其中 $Dst 和 $SharedPluginRoot 会被替换成实际路径。-> 只是普通箭头符号，让输出更好看