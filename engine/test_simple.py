import subprocess
import json

proc = subprocess.Popen(
    ['./meterea_engine'],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True
)
stdout, stderr = proc.communicate(input='{"command":"init"}\n')
print(f"STDOUT: {stdout}")
print(f"STDERR: {stderr}")
try:
    result = json.loads(stdout.strip())
    print(f"PARSED: {result}")
    print(f"status type: {type(result.get('status'))}")
except Exception as e:
    print(f"PARSE ERROR: {e}")
