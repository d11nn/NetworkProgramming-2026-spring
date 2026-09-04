# NP Project 4 SOCKS4 測試報告

## 測試環境

- Student ID: `314581047`
- Demo script: `./demo.sh 314581047`
- NP Server: `nplinux4.cs.nycu.edu.tw`
- SOCKS Server port: 每次 demo 重跑會變動，實測包含 `17244`, `32415`
- Browser: Firefox
- FTP Client: FlashFXP on Windows
- FTP Server for BIND test: `140.113.216.xx:21`
- `socks.conf` 測試設定：

```bash
printf 'permit c *.*.*.*\npermit b *.*.*.*\n' > socks.conf
```

## Spec 違規項檢查

| 項目 | 結論 | 證據 |
|---|---|---|
| 使用 Boost.Asio | 符合 | `socks_server.cpp`, `console_components.hpp` 均使用 `#include <boost/asio.hpp>` |
| 使用 SOCKS4 Protocol | 符合 | Server 處理 `VN=4`, `CD=1/2`, reply `90/91` |
| 未使用額外第三方 C++ library | 符合 | `Makefile` 只 link `-lboost_system -pthread` |
| `make` 可編譯 | 符合 | demo 顯示 `[SUCCESS] Compilation completed!` |
| 輸出格式 | 符合 | 已修正為 spec 六行格式，無 `new connect` 或 debug 分隔線 |

Spec 要求的 log 格式：

```text
<S_IP>: ...
<S_PORT>: ...
<D_IP>: ...
<D_PORT>: ...
<Command>: CONNECT / BIND
<Reply>: Accept / Reject
```

## Part 1: SOCKS Server CONNECT

### 測試方式

Firefox 設定 SOCKS4 proxy：

```text
SOCKS Host: nplinux4.cs.nycu.edu.tw
Port: demo 顯示的 socks_server port
SOCKS v4
```

測試網站：

```text
https://www.nthu.edu.tw/
https://www.nycu.edu.tw/
```

### 結果

Firefox 透過 SOCKS server 可連線允許的網站，server terminal 會輸出多筆 CONNECT log。

範例：

```text
<D_PORT>: 443
<Command>: CONNECT
<Reply>: Accept
```

Firefox 開網站會載入多個 CSS/JS/image/CDN resources，因此出現多組 CONNECT log 是正常現象。

### 結論

Part 1 CONNECT 符合。

## Part 2: SOCKS Server BIND

### 測試方式

FlashFXP 設定：

```text
Connection type: FTP
Proxy server: SOCKS4
Proxy host: nplinux4.cs.nycu.edu.tw
Proxy port: demo 顯示的 socks_server port
Data Connection Mode: Active mode (PORT)
Passive mode: OFF
```


帳號密碼未記錄於報告中。

### 關鍵修正

一開始 FlashFXP 使用 Passive mode，因此 log 出現：

```text
PASV
227 Entering Passive Mode
```

這不會觸發 SOCKS BIND。

後來改成：

```text
Connection > FTP > Data Connection Mode: Active mode (PORT)
```

FlashFXP log 變成：

```text
PORT 140,113,17,64,180,253
200 PORT command successful.
MLSD
150 Starting data transfer.
226 Operation successful
List Complete
```

SOCKS server log 出現：

```text
<Command>: BIND
<Reply>: Accept
```

### 結論

Part 2 BIND 符合。已確認 Active FTP data connection 會透過 SOCKS4 BIND，且 directory listing 成功。

## Part 3: CGI SOCKS Client

### 測試方式

Browser proxy 關閉，直接開：

```text
http://nplinux4.cs.nycu.edu.tw/~c314581047/npdemo4/panel_socks.cgi
```

填入 demo 啟動的 `np_single_golden` servers，例如：

```text
nplinux4.cs.nycu.edu.tw:19692, t1.txt
nplinux4.cs.nycu.edu.tw:14821, t2.txt
nplinux4.cs.nycu.edu.tw:28715, t3.txt
```

SOCKS server：

```text
Host: nplinux4.cs.nycu.edu.tw
Port: demo 顯示的 socks_server port
```

### 結果

瀏覽器成功顯示 project 3 console output，包含 prompt、commands、command output。

SOCKS server log：

```text
<Command>: CONNECT
<Reply>: Accept
```

### 同步問題修正

`console_components.hpp` 已修正 prompt 被 TCP 拆包時可能卡住的問題。  
原本只在單次 read chunk 中搜尋 `% `；現在保留上一個 chunk 的尾字元，能偵測跨 chunk 的 prompt。

### 結論

Part 3 CGI SOCKS Client 符合。

## Part 4: Firewall Dynamic Reload

### 測試方式

不重啟 `socks_server`，直接修改 `socks.conf`：

```bash
printf 'permit c 140.114.*.*\npermit b *.*.*.*\n' > socks.conf
```

預期 NYCU / 非 NTHU 連線被 Reject。

再改成：

```bash
printf 'permit c *.*.*.*\npermit b *.*.*.*\n' > socks.conf
```

預期重新整理後變成 Accept。

### 程式依據

`socks_server.cpp` 每次 request 都會重新讀取 `socks.conf`，不是只在 server 啟動時讀取，因此符合 dynamic reload 要求。

### 結論

程式設計符合 dynamic firewall。建議正式 demo 前再錄一次 Reject -> 修改 `socks.conf` -> Accept 的畫面作為證據。


## 最終結論

| Part | 狀態 |
|---|---|
| Part 1 CONNECT | 通過 |
| Part 2 BIND | 通過 |
| Part 3 CGI SOCKS Client | 通過 |
| Part 4 Firewall | 程式符合，建議正式 demo 前再做一次動態展示 |
| make 編譯 | 通過 |
| Boost.Asio / SOCKS4 | 符合 |
| 無額外第三方 library | 符合 |
