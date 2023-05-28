#!/bin/bash

# 编译 kernel module 和 tool
make

# 生成测试 image
make test.img

# 加载 kernel module
sudo insmod revofs.ko

# 检查 kernel 消息
dmesg | tail

# 创建测试目录和测试镜像
mkdir -p test
dd if=/dev/zero of=test.img bs=1M count=50

# 使用mkfs.revofs工具创建文件系统
./mkfs.revofs test.img

# 挂载测试镜像
sudo mount -o loop -t revofs test.img test

# 检查挂载成功的 kernel 消息
dmesg | tail

# 执行一些文件系统操作
sudo chmod 777 ./test
echo "Hello World" > test/hello
cat test/hello
ls -lR

# 卸载 kernel mount point 和 module
sudo umount test
sudo rmmod revofs

