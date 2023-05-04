## breif

南大计算机网络协议开发实验4，简单网络协议栈实现，主要参考实现网络层ip协议和传输层tcp协议，网络层支持链路状态和距离向量协议的动态路由，传输层支持基于GBN的可靠传输

## build

编译和运行的方法，使用了四台主机(或虚拟机), 使用`make`编译. 

## run

为了运行程序:

1. 启动重叠网络进程:
  在每个节点上, 在./son下运行son
  4个节点上的son进程应在1分钟内启动好.
  等待直到你在所有节点上看到: `waiting for connection from SIP process.`
2. 启动网络进程: 
  在每个节点上, 等待直到你在所有节点上看到: `waiting for connection from STCP process.`
3. 启动传输层并运行应用程序:
  在一个节点上, 进入server目录并运行`./app_simple_app或./app_stress_app`
  在另一个节点上, 进入client目录并运行`./app_simple_app或./app_stress_app`

## terminate

为了终止程序，使用"kill -s 2 进程号"杀掉son进程和sip进程.如果程序使用的端口号已被使用, 程序将退出.

## TODO

传输效率太低，应修正参数，可能需要给收发加锁
