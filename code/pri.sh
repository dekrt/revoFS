#!/bin/bash

# 编译 kernel module 和 tool

make > /dev/null 2>&1

# 加载 kernel module

sudo insmod revofs.ko

sudo mkdir -p /mnt/test
dd if=/dev/zero of=test.img bs=1M count=50
./mkfs.revofs test.img
sudo mount -o loop -t revofs test.img /mnt/test

sleep 2
# 执行一些文件系统操作
echo -e "\n"
echo "准备完成，开始执行文件系统操作..."
sudo su <<EOF
echo -e "\n"
echo  "×××××××××××××××××××××××××××××××××××××××××××××××××"
# 在root下创建多个测试文件测试权限操作
    echo "在root用户权限下创建文件test1"
    echo "命令：echo \"OSCOMP2023\" > /mnt/test/test1"
    echo "OSCOMP2023" > /mnt/test/test1
    ls -lR /mnt/test
    echo -e "\n"
exit
EOF
echo "尝试在普通用户权限下写入test1文件"
echo "echo \"write test\" >>/mnt/test/hello" 
echo "write test" >>/mnt/test/hello 
echo "访问失败，权限不足"
echo  "×××××××××××××××××××××××××××××××××××××××××××××××××"
echo "权限操作验证成功"
sudo umount /mnt/test
sudo rmmod revofs
sleep 1
make clean > /dev/null 2>&1

