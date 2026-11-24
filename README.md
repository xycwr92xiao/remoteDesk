昕宇局域网远程工具，支持winddows、linux(UOS、银河麒麟;x86_64、arm及loongarch架构)
在windows10、11及UOS和银河麒麟两种linux x86_64、arm及loongarch三种架构等环境测试通过有苹果电脑及其它环境的可自行编译测试。
支持共享剪切板、单一文件的复制粘贴、文件传输功能。

默认的连接端口及密码是：5902 "changeme123"

如果你想改变默认的端口和密码，请修改：/opt/remoteDesk/start_server.sh
内容：
#!/bin/bash

export DISPLAY=:0

CURRENT_USER=$(who | grep -m 1 '( :0 )' | awk '{print $1}')

if [ -n "$CURRENT_USER" ]; then

export XAUTHORITY="/home/$CURRENT_USER/.Xauthority"

xhost +SI:localuser:root >/dev/null 2>&1 || true
fi

#这里的参数可设置新的端口和密码
exec /opt/remoteDesk/bin/remote_server_linux 5902 "changeme123"

根据设置的端口要放开对应的端口访问权限，例如服务端：

麒麟服务器默认使用 firewalld 管理防火墙。先查看当前规则：

sudo firewall-cmd --list-all
检查 ports: 行里有没有 5902/tcp。如果没有，需要手动放行并重新加载：

sudo firewall-cmd --permanent --add-port=5902/tcp
sudo firewall-cmd --reload

<img width="332" height="276" alt="lianjie" src="https://github.com/user-attachments/assets/df3279f9-5ab7-4539-9a70-082300c5c7be" />
<img width="1018" height="747" alt="uos1" src="https://github.com/user-attachments/assets/e1a638dd-817d-4004-943e-422a9f926be2" />

<img width="1018" height="747" alt="qilin" src="https://github.com/user-attachments/assets/3c8eb8d7-716a-46b7-b652-4cd0d479fbf2" />

<img width="1296" height="869" alt="uos2" src="https://github.com/user-attachments/assets/616aad36-f6a2-49b4-8a4c-2f13f24f6d00" />
<img width="1018" height="747" alt="qilin2" src="https://github.com/user-attachments/assets/f96ab4f0-091d-4382-8598-8158fa959cb4" />
