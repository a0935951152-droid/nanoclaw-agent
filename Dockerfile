FROM python:3.9-slim

WORKDIR /app

# 安裝 Flask, API 客戶端與處理上傳檔案所需的套件
RUN pip install --no-cache-dir Flask requests anthropic werkzeug

# 複製應用程式檔案
COPY app.py .
COPY templates/ templates/

# 建立容器內的掛載點 (非必須，但屬良好習慣)
RUN mkdir -p /app/shared_data

EXPOSE 5000

CMD ["python", "app.py"]