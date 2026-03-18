# 🦞 NanoClaw: Autonomous AI Agent for Raspberry Pi

NanoClaw 是一個輕量級、高度自主的 AI 代理人（Agent）系統，專為 Raspberry Pi 與邊緣設備設計。它的運作靈感來自於 Open Interpreter 與 OpenClaw，能夠在受限的安全環境中自動編寫程式、執行指令、安裝環境，並進行自我除錯。

## ✨ 核心特色 (Features)

* **🤖 雙重工具調用 (Tool Use)**：內建 `execute_python` 與 `execute_bash` 工具，AI 能夠自主寫 Python 腳本或下達 Linux 終端機指令（如 `apt-get`, `pip install`）。
* **🔄 全自動除錯迴圈 (Auto-Correction)**：當程式執行報錯時，NanoClaw 會自動讀取錯誤訊息（Error Traceback），修改程式碼並重新執行，最多可自我迭代 5 次。
* **🛡️ Docker 沙盒隔離 (Sandbox)**：所有操作皆在 Docker 容器內執行，透過 Volume 掛載 `/app/shared_data` 作為安全交換區，完全不影響宿主機（Raspberry Pi）的系統安全。
* **🧠 持久化實體記憶 (Persistent Memory)**：對話與思考過程會即時序列化存入硬碟 (`brain_memory.pkl`)。即使重啟容器，AI 也能無縫接軌繼續未完成的任務，並具備防爆 Token 的滑動視窗機制。
* **📺 終端機實況轉播 (Live Streaming)**：前端網頁支援 SSE (Server-Sent Events) 串流技術，動態展示 AI 思考、下指令與報錯的即時過程。
* **💰 即時計費儀表板**：內建 Token 消耗追蹤器，即時換算 API 使用成本。

## 🛠️ 技術棧 (Tech Stack)
* **Backend**: Python 3, Flask, Server-Sent Events (SSE)
* **Frontend**: Vanilla HTML/JS/CSS (深色終端機風格)
* **AI Model**: Anthropic API (`claude-3-5-haiku-latest` / `claude-haiku-4-5`)
* **Infrastructure**: Docker, systemd

## 🚀 快速啟動 (Quick Start)

1. **環境準備**：確保你的設備已安裝 Docker。
2. **啟動服務**：
   執行部署腳本（會自動建立映像檔並啟動容器）：
   ```bash
   chmod +x deploy.sh
   ./deploy.sh
開啟控制台：
打開瀏覽器，前往 http://<你的設備IP>:5000。

登入系統：

系統密碼預設為：pi

輸入你的 Anthropic API Key (sk-ant-...)。

📁 資料夾結構 (Directory Structure)
Plaintext
nanoclaw_project/
├── app.py              # Flask 後端核心 (Agent 邏輯、記憶體管理、API 串流)
├── Dockerfile          # 定義 Python 執行環境
├── deploy.sh           # 自動化部署與重啟腳本
├── nanoclaw.service    # systemd 服務設定檔 (開機自啟)
├── templates/          
│   ├── login.html      # 登入介面
│   └── chat.html       # 實況轉播與聊天介面
└── shared_data/        # ⚠️ 與 Agent 的安全檔案交換區 (請勿推送到 Git)
    ├── .gitkeep        # 保持資料夾結構
    ├── brain_memory.pkl# (動態生成) AI 的實體記憶檔
    └── ...             # (動態生成) 用戶上傳與 AI 產出的檔案