# kHTTPD：這次到底證實了什麼？

這次是實際把 kernel module 載入 Linux VM，再跑 wrk，沒有用模擬數據填表。正式主測試每個版本跑 5 輪，每輪 30 秒，client 都是 4 threads / 256 connections，回應都是同一個 12-byte Hello World!。

| 指標（五輪中位數） | 恢復的舊版基準 | 現在的修正版 |
|---|---:|---:|
| 每秒完成 requests | 13,389 | 178,780 |
| p50 | 18.260 ms | 0.694 ms |
| p99 | 34.360 ms | 1.910 ms |
| wrk socket errors | 五輪皆 0 | 五輪皆 0 |

每一輪都拿同輪的基準相除，吞吐提升中位數為 **13.34 倍**，範圍 **13.04–13.43 倍**。p50 降低約 **96.2%**、p99 降低約 **94.4%**，也是先逐輪算改善幅度再取中位數。環境是 4 vCPU 的 AMD EPYC Linux VM；client 與 server 共用整台 VM，走 loopback。這是「專案舊版對修正版」，分母不是 nginx 或 userspace HTTP server。

## 為什麼可以快這麼多？

想像每次買東西都要重新辦一次會員。舊版有類似情況：程式裡雖然有 keep-alive 的命名，實際傳出去卻是 Connection: close。client 收到後就關閉連線；下次 request 又重連，而舊 server 每接受一條連線又建立一個 kernel thread。

因此，大量時間花在「建立連線、建立 thread、關閉連線」，並沒有花在那個很小的回應本身。恢復正確 keep-alive 後，同一批連線和 thread 可以處理幾百萬個 requests。計數器也對得上：基準一輪約 40 萬個完成 requests、建立約 40 萬個 thread；恢復的優化版一輪超過 530 萬個完成 requests，只建立 257 個 thread。

現在的修正版還改成固定、可追蹤的 worker pool，加入 framing、deadline、socket shutdown 與 join。它的吞吐約為恢復優化版的 99%，同時解決原本 unload 可能讓仍在跑的 thread 使用已釋放資源的問題。

## 每項優化真的都有用嗎？

另一台 Intel Xeon 4 vCPU VM 做了獨立對照：十種設定，各跑三輪、每輪十秒。它自己的修正版／基準吞吐提升為 27.03 倍；這台更快的數字沒有拿來取代主測試。每個機制只跟同一台 VM、相同測試條件的對照組比較。

- **keep-alive：效果最大。** 保留逐筆 logging 與清 buffer 的情況下，單獨啟用持續連線，中位數提升 12.45 倍。
- **移除逐筆 URL logging：有額外收益。** 在該 keep-alive 設定下，吞吐提升約 1.95 倍，p99 中位數 20.980 → 0.847 ms。
- **四個 listener、memory pool、預配置：這次沒有證明穩定的吞吐優勢。** 連線已經持續重用時，accept 與配置 buffer 本來就很少發生。它們不能被拿來各自認領整個 11 倍提升。

這些效果取決於其他設定，不應把每項中位數乘起來當作必然總增益。

## 原本履歷的 11 倍怎麼看？

原始的 3,436.83 → 37,739.84 req/s、p50 74.80 → 2.99 ms、p99 113.04 → 12.06 ms 紀錄全部保留。從 Git history 也找到了完整的優化版本，以及之後造成三行程式碼截斷的提交。這支持「當時量到有效果，但後來上傳的程式碼壞了」的解釋；原始機器與執行檔沒留下，無法回頭精確認定當年的所有條件。

**現在有新的、可核對的實驗證明，這個專案確實能產生超過 11 倍量級的改善。** 但新機器不會恰好印出當年的 3.4k、37.7k。履歷最紮實的寫法是採用這次 5 × 30 秒的 13.3 倍結果，並把舊值列為歷史量測。兩組數字各自有自己的基準，不能交叉相除。

## 如何讓別人真的重現？

[重現方法](PERFORMANCE_METHOD.md) 固定 source commit、基準和參數。[完整報告](PERFORMANCE.md) 包含每一輪數據、原始 archive、SHA-256、CPU/kernel/compiler/wrk、錯誤數與驗證方式。重新彙總會檢查原始檔 checksum、缺少或重複輪次、body/status；不完整的實驗直接拒絕。

有一次前置長測試因舊版 idle thread 沒結束，無法卸載 module。我保留失敗原始紀錄，加入所有歷史對照組共用的 10 秒 socket timeout，然後整組重跑。正式結果只來自完整通過的實驗。

重跑 256 connections 時使用 **workers=512、shards=4**。新版 workers 是同時服務連線的容量；舊版 workers 是 acceptor 數量。若照舊指令填 workers=4，就會把新版限制在同時只處理四條連線。

這次證明的是小型固定回應、loopback、封閉迴路 wrk 負載下的優化成果。p50 是一半完成 requests 的延遲門檻，p99 是 99% 完成 requests 的延遲門檻；它們要搭配錯誤數解讀。這不代表任何硬體、TLS、檔案服務或一般 production HTTP 都保證同樣倍數。面試時能講清楚基準、主要機制、對照的無效果結果和這個適用範圍，比背誦一個倍數更有說服力。
