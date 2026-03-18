#!/bin/bash

PROJECT_DIR="/home/test/nanoclaw_project"
CONTAINER_NAME="nanoclaw_chat"
PORT="5000"

cd $PROJECT_DIR

# 確保共用資料夾存在並設定權限
mkdir -p shared_data
chmod 777 shared_data

echo "1. 清理舊有容器..."
docker rm -f $CONTAINER_NAME 2>/dev/null || true

echo "2. 建置 Docker 映像檔..."
docker build -t nanoclaw-flask-image .

echo "3. 啟動容器並掛載 Volume 與 Port..."
# 加入 -v 參數進行資料夾映射
docker run -d \
    --name $CONTAINER_NAME \
    --restart unless-stopped \
    -p $PORT:5000 \
    -v $PROJECT_DIR/shared_data:/app/shared_data \
    nanoclaw-flask-image

echo "部署完成！網頁服務與 Volume 掛載已啟動。"