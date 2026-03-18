from flask import Flask, render_template, request, session, redirect, url_for, jsonify, send_from_directory, Response, stream_with_context
from werkzeug.utils import secure_filename
import anthropic
import subprocess
import os
import time
import pickle
import json

app = Flask(__name__)
app.secret_key = 'your_secure_random_secret_key'
SYSTEM_PASSWORD = "pi"
SHARED_DIR = '/app/shared_data'
app.config['UPLOAD_FOLDER'] = SHARED_DIR
os.makedirs(SHARED_DIR, exist_ok=True)

MEMORY_FILE = os.path.join(SHARED_DIR, 'brain_memory.pkl')

SYSTEM_PROMPT = """
你是一個運行在樹莓派 Docker 容器內的頂級全自動 Agent，運作方式類似 OpenClaw。
你的工作目錄限制在 /app/shared_data/。

【你的核心能力與行為準則】：
1. 自動迭代與除錯：如果執行指令後發生錯誤 (Error)，請自動分析錯誤訊息，修正並再次執行。
2. 環境配置：你可以使用 execute_bash 執行 Linux 指令 (如 apt-get/pip)。
3. ⚠️ 啟動疑惑：只有當你陷入無限錯誤迴圈或完全不理解意圖時，才停止工具並回覆 [需要人工確認]。
"""

TOOLS = [
    {
        "name": "execute_python",
        "description": "在 Docker 容器內執行 Python 程式碼。遇到 ModuleNotFoundError 需先用 bash 執行 pip install。",
        "input_schema": {
            "type": "object",
            "properties": {"script_code": {"type": "string"}},
            "required": ["script_code"]
        }
    },
    {
        "name": "execute_bash",
        "description": "在 Docker 容器內執行 Shell 指令。最大執行時間為 180 秒。",
        "input_schema": {
            "type": "object",
            "properties": {"command": {"type": "string"}},
            "required": ["command"]
        }
    }
]

def load_memory():
    if os.path.exists(MEMORY_FILE):
        try:
            with open(MEMORY_FILE, 'rb') as f: return pickle.load(f)
        except: return []
    return []

def save_memory(history):
    # 🌟 修正：安全的滑動視窗。避免切斷 tool_use 與 tool_result 的配對
    while len(history) > 30:
        history.pop(0)
        # 確保開頭一定是最單純的 user text 訊息，絕對不能是 assistant 或 tool_result 開頭
        while len(history) > 0 and (history[0]["role"] != "user" or isinstance(history[0]["content"], list)):
            history.pop(0)
            
    with open(MEMORY_FILE, 'wb') as f: pickle.dump(history, f)

@app.route('/', methods=['GET', 'POST'])
def login():
    if request.method == 'POST':
        password = request.form.get('password')
        api_key = request.form.get('api_key')
        if password == SYSTEM_PASSWORD and api_key.startswith("sk-ant-"):
            session['anthropic_api_key'] = api_key
            return redirect(url_for('chat'))
        return "密碼錯誤或 API Key 格式不正確", 401
    return render_template('login.html')

@app.route('/chat')
def chat():
    if 'anthropic_api_key' not in session: return redirect(url_for('login'))
    files = os.listdir(app.config['UPLOAD_FOLDER'])
    return render_template('chat.html', files=files)

@app.route('/api/upload', methods=['POST'])
def upload_file():
    if 'anthropic_api_key' not in session: return jsonify({"error": "Unauthorized"}), 401
    if 'file' not in request.files: return jsonify({"error": "No file part"}), 400
    file = request.files['file']
    if file.filename == '': return jsonify({"error": "No selected file"}), 400
    if file:
        filename = secure_filename(file.filename)
        file.save(os.path.join(app.config['UPLOAD_FOLDER'], filename))
        return jsonify({"message": f"檔案 {filename} 已成功上傳！", "filename": filename})

@app.route('/files/<filename>')
def uploaded_file(filename):
    if 'anthropic_api_key' not in session: return "Unauthorized", 401
    return send_from_directory(app.config['UPLOAD_FOLDER'], filename)

@app.route('/api/message', methods=['POST'])
def send_message():
    if 'anthropic_api_key' not in session: return jsonify({"error": "Unauthorized"}), 401
    
    user_message = request.json.get('message')
    api_key = session['anthropic_api_key']

    def generate():
        chat_history = load_memory()
        if user_message.strip() == "/clear":
            if os.path.exists(MEMORY_FILE): os.remove(MEMORY_FILE)
            yield f"data: {json.dumps({'type': 'finish', 'reply': '🧹 **實體記憶已徹底清除**！我們重新開始吧。', 'tokens': 0})}\n\n"
            return

        file_context = "【目前 /app/shared_data/ 的檔案預覽】：\n"
        try:
            for filename in os.listdir(app.config['UPLOAD_FOLDER']):
                path = os.path.join(app.config['UPLOAD_FOLDER'], filename)
                if os.path.isfile(path) and filename.endswith(('.c', '.py', '.txt', '.md', '.csv', '.sh')):
                    with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                        file_context += f"--- {filename} ---\n```\n{f.read(1000)}\n...\n```\n\n"
        except: pass

        full_system_prompt = SYSTEM_PROMPT + "\n\n" + file_context
        chat_history.append({"role": "user", "content": user_message})

        try:
            client = anthropic.Anthropic(api_key=api_key)
            reply_output = ""
            iteration = 0
            turn_tokens = 0

            while iteration < 5:
                if iteration > 0: 
                    yield f"data: {json.dumps({'type': 'thinking', 'content': f'🧠 進行第 {iteration+1} 次深度思考與除錯中，請稍候...'})}\n\n"
                    time.sleep(3)
                else:
                    yield f"data: {json.dumps({'type': 'thinking', 'content': '🧠 正在分析任務與規劃步驟...'})}\n\n"

                response = client.messages.create(
                    model="claude-haiku-4-5",
                    max_tokens=2000,
                    system=full_system_prompt,
                    tools=TOOLS,
                    messages=chat_history
                )
                
                turn_tokens += response.usage.input_tokens + response.usage.output_tokens

                if response.stop_reason == "tool_use":
                    # 🌟 修正：先不要寫入硬碟！等工具跑完再一起寫入，確保原子性
                    temp_assistant_message = {"role": "assistant", "content": response.content}
                    tool_results = []
                    
                    for block in response.content:
                        if block.type == "text":
                            reply_output += block.text + "\n"
                            yield f"data: {json.dumps({'type': 'update', 'content': block.text + '<br>'})}\n\n"
                        elif block.type == "tool_use":
                            exec_result = ""
                            if block.name == "execute_python":
                                script_code = block.input["script_code"]
                                step_msg = f"\n🐍 **[嘗試執行 Python]**：\n```python\n{script_code}\n```\n"
                                reply_output += step_msg
                                yield f"data: {json.dumps({'type': 'update', 'content': step_msg})}\n\n"
                                
                                script_path = os.path.join(SHARED_DIR, "agent_temp.py")
                                with open(script_path, "w", encoding="utf-8") as f: f.write(script_code)
                                try:
                                    proc = subprocess.run(["python", "agent_temp.py"], capture_output=True, text=True, cwd=SHARED_DIR, timeout=60)
                                    exec_result = proc.stdout if not proc.stderr else proc.stdout + "\n[錯誤]:\n" + proc.stderr
                                    if not exec_result.strip(): exec_result = "執行成功，無輸出。"
                                except subprocess.TimeoutExpired: exec_result = "❌ 執行失敗：超時中斷。"
                                except Exception as ex: exec_result = f"執行失敗: {str(ex)}"
                                finally:
                                    if os.path.exists(script_path): os.remove(script_path)
                            
                            elif block.name == "execute_bash":
                                command = block.input["command"]
                                step_msg = f"\n💻 **[執行 Shell]**： `{command}`\n"
                                reply_output += step_msg
                                yield f"data: {json.dumps({'type': 'update', 'content': step_msg})}\n\n"
                                
                                try:
                                    proc = subprocess.run(command, shell=True, capture_output=True, text=True, cwd=SHARED_DIR, timeout=180)
                                    exec_result = proc.stdout if not proc.stderr else proc.stdout + "\n[錯誤或警告]:\n" + proc.stderr
                                    if not exec_result.strip(): exec_result = "指令執行成功，無輸出。"
                                except subprocess.TimeoutExpired: exec_result = "⚠️ 警告：指令執行超過 180 秒。"
                                except Exception as ex: exec_result = f"指令失敗: {str(ex)}"

                            res_msg = f"📊 **[結果]**：\n```text\n{exec_result}\n```\n"
                            reply_output += res_msg
                            yield f"data: {json.dumps({'type': 'update', 'content': res_msg})}\n\n"
                            
                            tool_results.append({"type": "tool_result", "tool_use_id": block.id, "content": exec_result})

                    # 🌟 修正：工具全部執行完畢後，把 assistant(呼叫) 與 user(結果) 一次性加進陣列並存檔
                    chat_history.append(temp_assistant_message)
                    chat_history.append({"role": "user", "content": tool_results})
                    save_memory(chat_history)
                    
                    iteration += 1

                else:
                    chat_history.append({"role": "assistant", "content": response.content})
                    save_memory(chat_history)
                    final_text = "".join([b.text for b in response.content if b.type == "text"])
                    reply_output += "\n" + final_text
                    
                    yield f"data: {json.dumps({'type': 'finish', 'reply': reply_output, 'tokens': turn_tokens})}\n\n"
                    break
            
            if iteration == 5:
                reply_output += "\n\n⚠️ **[系統警告]**：已連續嘗試 5 次未完成。請根據錯誤紀錄給予明確指示！"
                yield f"data: {json.dumps({'type': 'finish', 'reply': reply_output, 'tokens': turn_tokens})}\n\n"

        except Exception as e:
            yield f"data: {json.dumps({'type': 'error', 'content': str(e)})}\n\n"

    return Response(stream_with_context(generate()), mimetype='text/event-stream')

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000)