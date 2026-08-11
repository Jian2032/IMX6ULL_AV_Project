# STREAM-R1：HTTP 网络骨架

## 1. 本轮边界

本轮只验证：

```text
socket -> bind -> listen -> accept -> recv请求 -> 路由 -> send响应 -> close
```

暂时不打开 `/dev/video0`，不编码 JPEG，也不输出 multipart。访问
`/stream.mjpg` 会得到预期的 `503 Service Unavailable`，这不是故障，而是明确标识
MJPEG 要到 `STREAM-R3` 才启用。

这样分层的目的，是避免后续遇到“浏览器打不开”时同时怀疑网卡、HTTP、V4L2、
JPEG 和线程队列。

## 2. 编译

Ubuntu：

```sh
cd /home/book/Workspace/linux/nfs/project/http_mjpeg
make clean
make info
make

file http_mjpeg
arm-linux-gnueabihf-readelf -h http_mjpeg | \
grep -E 'Class|Data|Machine'
```

预期：零 warning/error，产物为 ARM ELF32、little-endian、EABI5。

## 3. 开发板启动

先确认网络：

```sh
ifconfig eth0
route -n
ping -c 4 192.168.1.10
```

再启动服务：

```sh
cd /home/sun/nfs/project/http_mjpeg
./http_mjpeg --help
./http_mjpeg 8080
```

预期看到：

```text
listen address  : 0.0.0.0:8080
board URL       : http://192.168.1.50:8080/
server state    : LISTENING
```

另开开发板终端可确认监听端口：

```sh
netstat -ltn 2>/dev/null | grep ':8080'
```

## 4. PC 端测试

Windows PowerShell 请使用 `curl.exe`，避免 `curl` 被解释为 PowerShell 别名：

```powershell
curl.exe -i http://192.168.1.50:8080/
curl.exe -i http://192.168.1.50:8080/health
curl.exe -i http://192.168.1.50:8080/status
curl.exe -i http://192.168.1.50:8080/not-found
curl.exe -i http://192.168.1.50:8080/stream.mjpg
```

预期状态码依次为：

```text
200  200  200  404  503
```

也应能在浏览器打开：

```text
http://192.168.1.50:8080/
```

## 5. 退出和重启

在服务终端按 `Ctrl+C`，预期输出 `[STOP]`、统计值并返回退出码 0：

```sh
echo "exit_code=$?"
./http_mjpeg 8080 1
```

第二条命令限制只处理一次请求。PC 再访问一次 `/health` 后，程序应输出 `[PASS]`
并自动退出。它能立即重新绑定 8080 端口，证明监听 socket 已释放，
`SO_REUSEADDR` 和清理流程正确。

## 6. 验收标准

- ARM 交叉编译零 warning/error；
- PC 能访问开发板 8080 端口；
- `/`、`/health`、`/status` 返回 200；
- 未知路径返回 404；
- `/stream.mjpg` 在本轮明确返回 503；
- 浏览器断开不会因 `SIGPIPE` 杀死服务；
- `Ctrl+C` 退出码为 0，随后能立即重启；
- 内核日志无 Oops、WARNING 和 FEC 异常。

## 7. 这一轮要掌握的调用链

```text
PC浏览器/curl
      |
      v
FEC eth0 -> TCP/IP协议栈 -> accept()
                            |
                            v
                         recv()
                            |
                            v
                      解析请求行和路径
                            |
                            v
                  send(HTTP头) -> send(响应体)
                            |
                            v
                    shutdown() -> close()
```

`send()` 不保证一次发送完整数据，因此代码必须循环；客户端提前关闭时可能返回
`EPIPE`，服务应只结束当前连接，不能退出整个进程。

## 8. 实机验收记录（2026-08-10）

- ARM GCC 4.9.4交叉编译零warning/error，产物为ARM ELF32、little-endian、EABI5；
- Windows物理网卡配置为`192.168.1.20/24`后，与开发板`192.168.1.50`连通；
- `/`、`/health`、`/status`返回200，未知路径返回404；
- `/stream.mjpg`按本轮设计返回503，POST按设计返回405；
- 状态接口报告`bad_requests=0`、`send_errors=0`；
- 限制1个请求运行时，`accepted=1`、`completed=1`、`bytes_sent=201`；
- 程序输出`[PASS]`并以0退出，随后端口可立即重新使用；
- `dmesg`无新增Oops、WARNING或FEC异常。

结论：`STREAM-R1`验收通过并冻结。
