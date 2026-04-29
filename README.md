# [STM32 OTA Update: APP part](https://github.com/hanserisangel/STM32-OTA-Update_APP-part)

----

## 1. 项目简介
本项目是一个基于 **STM32F407ZGT6** 的 OTA 远程升级的 APP 部分，需与我的另一个[Bootloader 项目](https://github.com/hanserisangel/STM32-OTA-Update_Bootloader-part)搭配使用，共同实现完整的全量/差分固件升级流程。

核心功能如下：
- ✅ **FreeRTOS 任务化架构**：将 OTA 升级作为独立任务实现，不阻塞主业务流程，方便移植与扩展。
- ✅ **HTTP 固件接收**：通过 ESP32-C3 模块的 AT 指令与 OneNET 服务器通信，基于 UART 空闲中断 + DMA + RingBuffer 实现高效、无阻塞的数据收发。
- ✅ **断点续传**：固件接收进度实时记录在 W25Q64 中，断电或复位后可从中断处继续接收，无需从头开始传输。
- ✅ **外部 Flash 优化**：内置**磨损均衡、垃圾回收、掉电保护**，通过**二分查找维护扇区有序性**，提升断电后数据重建效率，提供类文件系统的可靠存储体验。
- ✅ **差分升级支持**：可配合 Bootloader 的 HPatchLite + tinyuz 方案，实现增量升级，大幅减少升级包体积与传输时间。

### 1.1 ONENET 服务器
[ONENET 服务器](https://open.iot.10086.cn/console/summary)是中国移动旗下的物联网平台，截至目前都是免费使用的，注册好账号后，需要先注册设备，再开始 OTA 升级服务，具体流程如下：

1. **创建设备**

    - 先创建产品
        <div style="text-align: center;">
            <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E5%88%9B%E5%BB%BA%E4%BA%A7%E5%93%81.png" width="100%" height="100%" alt="创建产品">
        </div>
    
    - 然后再创建设备
        <div style="text-align: center;">
            <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E5%88%9B%E5%BB%BA%E8%AE%BE%E5%A4%87.png" width="100%" height="100%" alt="创建设备">
        </div>

    > **关键参数**
    - Product_ID：产品 ID
        <div style="text-align: center;">
            <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E4%BA%A7%E5%93%81ID.png" width="100%" height="100%" alt="产品 ID">
        </div>
    - Device_ID：设备 ID（你创建设备时起的名字）
        <div style="text-align: center;">
            <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E8%AE%BE%E5%A4%87ID.png" width="100%" height="100%" alt="设备 ID">
        </div>
    - 设备密钥
        <div style="text-align: center;">
            <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E8%BF%9B%E5%85%A5%E8%AE%BE%E5%A4%87%E8%AF%A6%E6%83%85.png" width="100%" height="100%" alt="进入设备详情">
        </div>
        <div style="text-align: center;">
            <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E5%AF%86%E9%92%A5%E4%BF%A1%E6%81%AF.png" width="100%" height="100%" alt="密钥信息">
        </div>

    > **Token 的生成**
    
    利用[官方工具](https://open.iot.10086.cn/doc/aiot/fuse/detail/1487)生成，这里选设备级鉴权，把之前获得的 3 个关键参数填写到对应位置。需要说明的是`et`怎么获取，`et`是一个未来时间的时间戳，你可以使用一个时间戳转换网站获取，记住，一定要是未来的时间，到 2050 年都没事。(下图截取自 ONENET 官方文档)
    <div style="text-align: center;">
        <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E8%8E%B7%E5%8F%96token.png" width="70%" height="70%" alt="获取 Token">
    </div>
    
    生成以后可以把这些参数复制到程序中`OTA.h`文件的宏定义中，如下所示：
    ```c
    #define Product_ID "qvt81NOIxc"
    #define Device_ID "microwave"
    #define Token "version=2018-10-31&res=products%2Fqvt81NOIxc%2Fdevices%2Fmicrowave&et=1802007194&method=md5&sign=cI8Xo%2BL9Ef3vNNTRtYNufQ%3D%3D"
    ```
2. **OTA 升级（增值服务）**

    - 创建升级包：
        <div style="text-align: center;">
            <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E5%88%9B%E5%BB%BA%E5%8D%87%E7%BA%A7%E5%8C%85.png" width="100%" height="100%" alt="创建升级包">
        </div>
    - 添加升级包
        <div style="text-align: center;">
            <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E6%B7%BB%E5%8A%A0%E5%8D%87%E7%BA%A7%E5%8C%85.png" width="100%" height="100%" alt="添加升级包">
        </div>
    - 跳过验证升级
        <div style="text-align: center;">
            <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E8%B7%B3%E8%BF%87%E9%AA%8C%E8%AF%81.png" width="100%" height="100%" alt="跳过验证">
        </div>

- **不管上传的是差分包还是全量包，在添加 OTA 升级任务的时候都要选择完整包，不用管差分包选项**
- **本项目中这里的差分包或者全量包并不是由 keil5 直接生成的 bin 固件，而是经过打包后的固件包，具体如下小节所示**

### 1.2 固件包的格式

<div style="text-align: center;">
    <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/OTAremote.drawio.svg" width="100%" height="100%" alt="固件包格式">
</div>

说明：
1. **头部**：20 byte, 包含`魔数(4 字节)、包头长度(4 字节)、包类型(4 字节)、固件长度(4 字节)、签名长度(4 字节)`
   - 包头长度：20
   - 包类型：0 是全量包，1 是增量包
   - 固件长度：固件大小
   - 签名长度：固件包最后一个字段`签名`的长度
2. **ECDH公钥+盐值+iv**：65 + 16 + 16 = 97 byte，用于派生 AES 密钥
3. **固件密文**：AES 加密后的固件
4. **签名**：ECDSA 签名值

### 1.3. 固件包的打包方法
本部分全部与 [bootloader 项目](https://github.com/hanserisangel/STM32-OTA-Update_Bootloader-part)一致，请移步查看。
### 1.4 生成差分压缩固件
本部分全部与 [bootloader 项目](https://github.com/hanserisangel/STM32-OTA-Update_Bootloader-part)一致，请移步查看。

---

## 2. 环境准备

以下是本项目开发与验证所使用的环境，并非强制要求，你可以根据自身情况灵活选择。

#### 推荐开发环境
- **操作系统**：Windows 11
- **开发工具链**：
  - IDE：Keil MDK-ARM 5
  - 配置工具：STM32CubeMX
  - 辅助脚本：Python 3
  - 串口工具：SecureCRT
- **密钥生成工具**：Ubuntu 22.04 + OpenSSL
- **硬件平台**：STM32F407ZGT6 + W25Q64 SPI Flash + ESP32-C3

#### 强制要求
以下两点是运行本项目的硬性前提：
1.  **MCU 内部 Flash 空间**：必须大于 `Bootloader大小 + 2 × APP分区大小`（本项目当前 Bootloader 体积约 66KB，裁剪功能可进一步减小体积；APP 分区大小可根据实际需求调整）。
2.  **外部 Flash**：必须搭载 SPI Flash（本项目使用 W25Q64），用于缓存固件升级包和其他关键参数。

---

## 3. 理论介绍
### 3.1 中断向量表映射
由于A/B分区的APP固件不是从`0x0800_0000`地址开始存放的，其内部的中断向量表地址也发生了偏移。为了让内核在 APP 运行时能正确找到中断服务程序，必须修改中断向量表的映射地址：
1.  **核心机制**：Cortex-M 内核的`SCB`（系统控制块）中，有一个名为`VTOR`（向量表偏移寄存器）的寄存器，它决定了中断向量表的基地址。
2.  **实现方法**：在跳转前，将`VTOR`寄存器的值设置为目标 APP 的起始地址。这样，内核在响应中断时，就会从APP程序的起始位置去查找中断向量表，确保中断功能正常工作。

    <div style="text-align: center;">
        <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E5%90%91%E9%87%8F%E8%A1%A8%E6%98%A0%E5%B0%84.png" width="80%" height="80%" alt="向量表映射">
    </div>
修改文件中的这个值，完成映射。这里我写 `0x20000` 是因为这是 A 分区的 APP 程序，根据第 5 节的分区表， A 分区在 128KB 开始的区域；如果是 B 分区的 APP 程序，这里要填 `0x80000`，因为 B 分区在 128+384KB 开始的区域

如果使用 keil5 直接下载 A/B 分区的应用程序，还需要修改下图的值
    <div style="text-align: center;">
        <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/keil_App_A.png" width="80%" height="80%" alt="keil_APP_A">
    </div>
    <div style="text-align: center;">
        <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/keil_App_B.png" width="80%" height="80%" alt="keil_APP_B">
</div>

### 3.2 http 协议

本项目的 OTA 远程升级采用 **HTTP 协议** 实现，因为 ONENET 服务器只能使用 HTTP 协议进行 OTA 升级，具体流程如下所示：
1. **上报设备当前的版本号**
```http
POST /fuse-ota/{Product_ID}/{Device_ID}/version HTTP/1.1
Content-Type:application/json
Authorization:{Token}
Host:iot-api.heclouds.com
Content-Length:53

{"s_version":"版本号","f_version":"版本号"}
```
`Content-Length`这个字段是 HTTP 报文的数据长度。`{"s_version":"version-1.0","f_version":"version-1.0"}`长度为 53 所以`Content-Length`是 53。程序中不是写死的长度，所以如果想要修改版本号的长度也不用担心。

服务器的应答报文：
```http
HTTP/1.1 200 OK
Date: Tue, 10 Feb 2026 09:48:03 GMT
Content-Type: application/json;charset=UTF-8
Content-Length: 71
Connection: keep-alive
Access-Control-Allow-Headers: *
Access-Control-Allow-Origin: *
Server: nginx
Pragma: no-cache

{"code":0,"msg":"succ","request_id":"7a0ac947dfc044f1bf89e9c9095481f4"}
```
程序中就是通过获取`code`字段的值是否为 0 来判断上报版本号是否成功。

2. **检测升级任务**
```http
GET /fuse-ota/{Product_ID}/{Device_ID}/check?type=2&{版本号} HTTP/1.1
Content-Type:application/json
Authorization:{Token}
Host:iot-api.heclouds.com
\r\n
```
ONENET 服务器需要设备发送报文询问是否有升级任务，以后可以用软件定时器每个一段时间发送这个报文询问是否有升级任务。

如果有升级任务，服务器的应答报文：
```http
HTTP/1.1 200 OK
Date: Tue, 10 Feb 2026 09:53:48 GMT
Content-Type: application/json;charset=UTF-8
Content-Length: 209
Connection: keep-alive
Access-Control-Allow-Headers: *
Access-Control-Allow-Origin: *
Server: nginx
Pragma: no-cache

{"code":0,"msg":"succ","data":{"target":"{创建升级任务时你填的版本号}","tid":1309304,"size":15648,"md5":"0d359d2e5a810712fd667d1ee3495ad6","status":1,"type":1},"request_id":"dc69a00d391749febdf221181abd7708"}
```

- `target`：这个升级任务的固件版本号，设备端需要获取写入 W25Q64
- `tid`：这个升级任务的 ID，之后下载固件的时候需要用。
- `size`：这个升级任务的固件大小，设备端需要获取写入 W25Q64

3. **分片下载**
```http
GET /fuse-ota/{Product_ID}/{Device_ID}/{tid}/download HTTP/1.1
Content-Type: application/json
Authorization:{Token}
host:iot-api.heclouds.com
Range:0-511
```
`Range`：这个字段是表示本轮设备端请求的固件范围。比如一个固件 1024B，那么设备端第一次发送`Range:0-511`，第二次发送`Range:512-1023`，分两次下载完固件。

服务器应答报文：
```http
HTTP/1.1 200 OK
Date: Tue, 10 Feb 2026 09:54:09 GMT
Content-Type: application/octet-stream;charset=UTF-8
Content-Length: 15648
Connection: keep-alive
Access-Control-Allow-Headers: *
Access-Control-Allow-Origin: *
Content-Disposition: attachment; filename="dada"
Ota-Errno: 0
Server: nginx
Pragma: no-cache

[512B的固件]
```
**流程结束**

### 3.3 串口空闲中断 + DMA + ringbuffer
本项目的串口数据接收采用 **空闲中断 + DMA + RingBuffer** 的组合方案，兼顾了效率、CPU负载与灵活性

#### 方案选型说明
1.  **为什么不需要双缓冲接收？**
    因为目前的传输方式是：设备发送 HTTP 报文，服务器才会下发 512 byte 固件，设备不发送报文，服务器则不会下发数据。因此不会出现发送方持续发送导致接收缓冲区溢出的情况，无需复杂的双缓冲机制。

2.  **为什么使用空闲中断？**
    串口除了接收 HTTP 固定长度的数据帧外，还需要接收不定长的 ESP32 AT 指令的返回值。空闲中断可以在传输停止时自动触发，判断一帧数据接收完成，适配不定长数据的处理场景。

3.  **为什么使用DMA？**
    DMA 控制器可以在无需 CPU 干预的情况下，直接将串口数据搬运到内存中，大幅减轻CPU负担，实现高效、低延迟的数据接收。

4.  **为什么使用RingBuffer？**
    环形缓冲区作为数据接收的中间缓存，可以平滑处理“高速接收”与“低速处理”之间的速度差，提高内存利用率，避免数据丢失。

### 3.4 A/B 分区与自动回滚
除了 bootloader 部分，stm32 的内部 flash 分成了 A/B 双分区，一旦新固件在设备上运行失败，则重启后回滚到另一个分区，防止设备变砖，自动回滚逻辑如下：
```c
bool OTA_ConfirmBootSuccess(void)
{
    if(OTA_Info.OTA_status != UPDATE)
        return false;

    OTA_Info.OTA_area = OTA_GetCurrentSlotByVTOR();
    OTA_Info.OTA_status = NORMAL;   // 最关键
    OTA_Info.OTA_Flag = 0U;
    W25Q64_WriteOTAInfo();  // 保存进 W25Q64

    return true;
}
```
APP 固件程序需要在程序一开始向 W25Q64 写入`OTA_Info.OTA_area = NORMAL`，下次重启时才不会进行回滚。

### 3.5 断点续传
在固件接收过程中，若出现断电或复位，设备恢复后可从上次中断位置继续接收，无需从头开始。该功能将下载进度持久化到 W25Q64，并实现了**磨损均衡、掉电保护、垃圾回收**三大机制。

#### 3.5.1 flash 磨损均衡

#### 3.5.2 flash 垃圾回收

#### 3.5.3 flash 掉电保护

#### 3.5.4 flash 数据重建

---

## 4. demo 演示
### 4.1 版本号上报
<div style="text-align: center;">
    <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E4%B8%8A%E6%8A%A5%E7%89%88%E6%9C%AC%E5%8F%B7.png" width="100%" height="100%" alt="版本号上报">
</div>

### 4.2 检测升级任务
<div style="text-align: center;">
    <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E6%A3%80%E6%B5%8B%E5%8D%87%E7%BA%A7%E4%BB%BB%E5%8A%A1.png" width="100%" height="100%" alt="检测升级任务">
</div>

### 4.4 分片接收
<div style="text-align: center;">
    <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E6%89%B9%E9%87%8F%E5%8D%87%E7%BA%A7.png" width="100%" height="100%" alt="批量升级">
</div>
<div style="text-align: center;">
    <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E5%88%86%E7%89%87%E4%B8%8B%E8%BD%BD.png" width="70%" height="70%" alt="分片接收">
</div>

**开始下载**

### 4.5 断点续传
<div style="text-align: center;">
    <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E6%96%AD%E7%82%B9%E7%BB%AD%E4%BC%A01.png" width="50%" height="50%" alt="断点续传1">
</div>
<div style="text-align: center;">
    <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E6%96%AD%E7%82%B9%E7%BB%AD%E4%BC%A02.png" width="70%" height="70%" alt="断点续传2">
</div>
下载到 10752 处时，我按了复位，接收过程中断。复位后继续下载固件，回退了一段，从 10240 处开始下载

### 4.6 结果
<div style="text-align: center;">
    <img src="https://github.com/hanserisangel/STM32-OTA-Update_APP-part/blob/master/image/%E5%8D%87%E7%BA%A7%E6%88%90%E5%8A%9F.png" width="90%" height="90%" alt="升级成功">
</div>

本实例是通过 A 区的 APP 下载固件，并把固件经过 W25Q64 后写入 APP_B 区，所以图片中是从 B 区开始执行。因此，我上传上去的固件包的中断向量表偏移是`0x80000`；如果是反过来，通过 B 区的 APP 下载固件，那么上传上去的固件包的中断向量表偏移是`0x20000`。另外，进度条没满不用在意，因为最后一段不满 512 字节的部分没有串口打印出来。

**注意**
- **差分升级用到了旧固件和差分固件两个输入来源，旧固件直接读取 mcu 内部 flash 的活动分区，差分固件则是本地/远程下载来的。因此，进行差分升级的时候，务必要保证 mcu 内部 flash 的活动分区运行的是旧固件，要不然差分还原的结果是错的，得到的新固件也是运行不起来的。**
- **一定要明确升级的新固件是运行在 A 区还是 B 区的，因为中断向量表的映射不一样。如果你升级的固件是 A 区的，但是被下载进 B 区了，那程序就运行不起来**

---

## 5. FLASH 分区表
### 5.1 W25Q64

<table style="width:100%">
    <tr>
        <td style="text-align:center;background:#F2F2F2;width:200px"><b>起始位置(字节)</b></td>
        <td style="text-align:center;background:#F2F2F2;width:200px"><b>结束位置(字节)</b></td>
        <td style="text-align:center;background:#F2F2F2;width:200px"><b>分区大小</b></td>
        <td style="text-align:center;background:#F2F2F2;width:400px"><b>存放的数据</b></td>
        <td style="text-align:center;background:#F2F2F2;width:200px"><b>数据大小</b></td>
        <td style="text-align:center;background:#F2F2F2;width:100px"><b>是否为热数据</b></td>
    </tr>
    <tr>
        <td style="text-align:center">0</td>
        <td style="text-align:center">4KB</td>
        <td style="text-align:center">4KB</td>
        <td style="text-align:center">ECDSA 公钥</td>
        <td style="text-align:center">91 byte</td>
        <td style="text-align:center">&#10062</td>
    </tr>
    <tr>
        <td style="text-align:center">4KB</td>
        <td style="text-align:center">8KB</td>
        <td style="text-align:center">4KB</td>
        <td style="text-align:center">OTA_Info 信息</td>
        <td style="text-align:center">28 byte</td>
        <td style="text-align:center">&#10062</td>
    </tr>
    <tr>
        <td style="text-align:center">8KB</td>
        <td style="text-align:center">12KB</td>
        <td style="text-align:center">4KB</td>
        <td style="text-align:center">mata元数据（ecdh公钥+盐值+iv）</td>
        <td style="text-align:center">97 byte</td>
        <td style="text-align:center">&#10062</td>
    </tr>
    <tr>
        <td style="text-align:center">12KB</td>
        <td style="text-align:center">16KB</td>
        <td style="text-align:center">4KB</td>
        <td style="text-align:center">包头+签名数据</td>
        <td style="text-align:center">20+70~20+72 byte</td>
        <td style="text-align:center">&#10062</td>
    </tr>
    <tr>
        <td style="text-align:center">16KB</td>
        <td style="text-align:center">0x10000</td>
        <td style="text-align:center">1008KB</td>
        <td style="text-align:center">APP断点续传的进度</td>
        <td style="text-align:center">不定</td>
        <td style="text-align:center">&#9989</td>
    </tr>
    <tr>
        <td style="text-align:center">0x10000</td>
        <td style="text-align:center">0X70000</td>
        <td style="text-align:center">384KB</td>
        <td style="text-align:center">加密固件</td>
        <td style="text-align:center">固件大小</td>
        <td style="text-align:center">&#10062</td>
    </tr>
</table>

### 5.2 mcu flash

<table style="width:100%">
    <tr>
        <td style="text-align:center;background:#F2F2F2;width:200px"><b>存放的数据</b></td>
        <td style="text-align:center;background:#F2F2F2;width:200px"><b>起始位置(字节)</b></td>
        <td style="text-align:center;background:#F2F2F2;width:200px"><b>结束位置(字节)</b></td>
        <td style="text-align:center;background:#F2F2F2;width:100px"><b>分区大小</b></td>
        <td style="text-align:center;background:#F2F2F2;width:200px"><b>扇区(编号)</b></td>
    </tr>
    <tr>
        <td style="text-align:center"><b>bootloader</b></td>
        <td style="text-align:center">0x0800_0000</td>
        <td style="text-align:center">0x0802_0000</td>
        <td style="text-align:center">128KB</td>
        <td style="text-align:center">0-4</td>
    </tr>
    <tr>
        <td style="text-align:center"><b>APP 分区 A</b></td>
        <td style="text-align:center">0x0802_0000</td>
        <td style="text-align:center">0x0808_0000</td>
        <td style="text-align:center">384KB</td>
        <td style="text-align:center">5-7</td>
    </tr>
    <tr>
        <td style="text-align:center"><b>APP 分区 B</b></td>
        <td style="text-align:center">0x0808_0000</td>
        <td style="text-align:center">0x080e_0000</td>
        <td style="text-align:center">384KB</td>
        <td style="text-align:center">8-10</td>
    </tr>
</table>
