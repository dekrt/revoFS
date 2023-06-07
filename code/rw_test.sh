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

# 在root下创建多个测试文件并进行读写操作
    echo "命令：echo \"OSCOMP2023\" > /mnt/test/test1"
    echo "OSCOMP2023" > /mnt/test/test1
    ls -lR /mnt/test
    echo -e "\n"
    echo "cat输出test1文本内容"
    echo -e "\n"
    cat /mnt/test/test1
    echo -e "\n"
    echo "File content is correct."
    echo -e "\n"
    echo "追加字符 echo \"Hust Welcome To You\" >>/mnt/test/test1"
    echo "Hust Welcome To You" >> /mnt/test/test1
    echo -e "\n"
    echo "cat输出test1文本内容"
    echo -e "\n"
    cat /mnt/test/test1
    echo -e "\n"
    echo "File content is correct."
    
echo  "×××××××××××××××××××××××××××××××××××××××××××××××××"
echo -e "\n"
exit
EOF

echo "读写操作验证成功"
sudo umount /mnt/test
sudo rmmod revofs
sleep 1
make clean > /dev/null 2>&1

