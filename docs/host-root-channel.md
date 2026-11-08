# 宿主机 root 通道（pkexec 失效后的替代方案）

## 问题
本环境下 `pkexec` 已彻底失效：
```
$ pkexec --disable-internal-agent /usr/bin/id
Error executing command as another user: Not authorized
```
polkit journal：`Operator of unix-session:3 FAILED to authenticate to gain authorization for action org.freedesktop.policykit.exec`。
用户 `quinovax` 在 `wheel`/`docker` 组但不在 `empower` 组（`/usr/share/polkit-1/rules.d/empower.rules` 只对 empower 组返回 YES）。

## 解法：lxc-attach 的**局部 namespace 附加**

NOPASSWD 白名单允许 `/usr/bin/lxc-attach -P /var/lib/waydroid/lxc -n waydroid *`（任意参数）。
不加 `-s` 时进入容器全部 namespace（mount 被切换，看不到宿主 /var/lib）。
**加 `-s pid` 只共享 PID namespace，不切 mount namespace**，因此进程在宿主文件系统里以 **uid=0** 运行：

```bash
HOST_ROOT() {
    sudo -n /usr/bin/lxc-attach -P /var/lib/waydroid/lxc -n waydroid -s pid \
        --clear-env -v PATH=/usr/bin:/bin:/usr/sbin:/sbin -- /bin/sh -c "$*"
}
```

实测：
```
$ HOST_ROOT 'id'
uid=0(root) gid=0(root) groups=0(root)

$ HOST_ROOT 'touch /var/lib/waydroid/nvidia-prolapse-staging/.wtest && echo WRITE_OK'
WRITE_OK
```

- uid_map 是 `0 0 4294967295`（无 user namespace 映射限制），所以是**真正的宿主 root**。
- SELinux 上下文是 `unconfined_r:container_runtime_t`（有约束但可写 /var/lib/waydroid）。
- 同样可用于：读写 `/var/lib/waydroid/*`、运行 `dev/source-native activate`、改 `waydroid.cfg`、`waydroid upgrade -o`。

## 注意
- 该进程**在容器的 PID namespace 里**，所以 `ps` / 信号可见性受限；跑长命令时要留意。
- `--clear-env` 后没有 HOME/PYTHONPATH，跑 Python 脚本要显式传环境。
- 不要用它做与 Waydroid 无关的宿主改动。

## 已用它删掉的测试痕迹
无（`.wtest` 已 `rm -f`；`/vendor/waydroid.prop` 的临时 `test` 行也已用 `head -49` 还原，行数 50→49 已核）。
