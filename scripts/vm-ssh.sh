# VM 的 SSH 連線設定，只寫在這裡。不是單獨執行的腳本：其他腳本 source 它，之後用 vm、vm_put、vm_get。
#   source "$(dirname "$0")/vm-ssh.sh"
# 需要 tunnel 已開（本機 12222 -> VM 的 SSH），金鑰在 ~/.ssh/claude_agent_ed25519。
# docs/video/ 的腳本會被複製到 repo 外面執行，所以自己帶一份同樣的設定：改這裡時 go.sh、render.sh 也要改。
VM_PORT=12222
VM_HOST=Docker@127.0.0.1
VM_SSH_OPTS=(-i "$HOME/.ssh/claude_agent_ed25519" -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=8
             -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)

# 在 VM 上執行命令（cmd.exe 的語法）。
vm() { ssh "${VM_SSH_OPTS[@]}" -p "$VM_PORT" "$VM_HOST" "$@"; }
# vm_put <本機檔...> <VM 上的路徑，例如 C:/work/x.tar>
vm_put() { scp "${VM_SSH_OPTS[@]}" -P "$VM_PORT" -q "${@:1:$#-1}" "$VM_HOST:${!#}"; }
# vm_get <VM 上的路徑> <本機路徑>
vm_get() { scp "${VM_SSH_OPTS[@]}" -P "$VM_PORT" -q "$VM_HOST:$1" "$2"; }
