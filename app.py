from flask import Flask, jsonify, render_template

app = Flask(__name__)

trigger = False

# 🌐 Serve the webpage
@app.route("/")
def home():
    return render_template("index.html")

@app.route("/status", methods=["GET"])
def status():
    return jsonify({"trigger": trigger})

# 🔴 Trigger endpoint
@app.route("/trigger", methods=["POST"])
def set_trigger():
    global trigger
    trigger = True
    return jsonify({"status": "trigger set"})

# 🟢 Device polls here
@app.route("/check", methods=["GET"])
def check_trigger():
    global trigger
    value = trigger
    trigger = False
    return jsonify({"trigger": value})

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=10000)