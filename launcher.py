import subprocess
import time
import sys

def run_deployment():
    print("🚀 [NanoClaw] 正在啟動部署腳本...")
    try:
        # 執行 shell 腳本並將輸出實時顯示
        process = subprocess.Popen(['bash', 'deploy.sh'], 
                                   stdout=subprocess.PIPE, 
                                   stderr=subprocess.STDOUT, 
                                   text=True)
        for line in process.stdout:
            print(line.strip())
        process.wait()
        
        if process.returncode == 0:
            print("✅ [NanoClaw] Docker 部署與通道設定成功！")
        else:
            print(f"❌ [NanoClaw] 部署失敗，退出碼: {process.returncode}")
            
    except Exception as e:
        print(f"⚠️ [NanoClaw] 執行過程發生錯誤: {e}")

if __name__ == "__main__":
    # 延遲幾秒確保網路與 Docker 引擎完全就緒
    time.sleep(5)
    run_deployment()
    
    # 保持 Python 進程存活 (如果系統需要監控容器，可在此實作無限迴圈)
    while True:
        time.sleep(3600)