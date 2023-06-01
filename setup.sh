#!/bin/bash

# 编译 kernel module 和 tool
echo "开始编译 kernel module 和 tool..."
make
echo "编译完成！"
echo -e "\n"
sleep 1

# 生成测试 image
echo "开始生成测试 image..."
make test.img
echo "测试 image 生成完成！"
echo -e "\n"
sleep 1
# 加载 kernel module

echo "开始加载 kernel module..."
sudo insmod revofs.ko
echo "kernel module 加载完成！"
echo -e "\n"
sleep 1

# 创建测试目录和测试镜像
echo "开始创建测试目录和测试镜像..."
sudo mkdir -p /mnt/test
dd if=/dev/zero of=test.img bs=1M count=50
echo "测试目录和测试镜像创建完成！"
echo -e "\n"
sleep 1

# 使用mkfs.revofs工具创建文件系统
echo "开始使用 mkfs.revofs 工具创建文件系统..."
./mkfs.revofs test.img
echo "文件系统test.img创建完成！"
echo -e "\n"
sleep 1

# 挂载测试镜像
echo "开始挂载测试镜像..."
sudo mount -o loop -t revofs test.img /mnt/test
echo "测试镜像挂载到/mnt/test！"
echo -e "\n"
sleep 1

# 检查挂载成功的 kernel 消息
#dmesg | tail

# 执行一些文件系统操作
echo "开始执行文件系统操作..."
sudo su <<EOF
echo -e "\n"
echo "在root下创建hello文本并输入字符串"
echo -e "\n"
echo "OSCOMP 2023 " > /mnt/test/hello
ls -lR /mnt/test
echo -e "\n"
echo "cat输出文本内容"
echo -e "\n"
cat /mnt/test/hello
echo -e "\n"
exit
EOF

echo "在普通用户下尝试对hello进行写操作"
echo "echo \"OSCOMP 2023\" > /mnt/test/hello" 
echo "OSCOMP 2023 " > /mnt/test/hello
echo "权限设置测试成功"
echo -e "\n"
sleep 1
echo "文件系统操作完成！"
echo -e "\n"
sleep 1
# 卸载 kernel mount point 和 module
echo "开始卸载 kernel mount point 和 module..."
sudo umount /mnt/test
sudo rmmod revofs
echo "kernel mount point 和 module 卸载完成！"
echo -e "\n"
sleep 1
echo "开始清理构建环境..."
make clean
echo "构建环境清理完成！"

