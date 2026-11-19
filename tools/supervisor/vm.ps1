# vm - run a command in / open a shell on the supervisor's first running VM.
# PowerShell passes @args as an array, so quoted commands survive intact:
#   vm -c 'echo "a b"; uname -n'
# (vm.cmd, for cmd.exe, re-parses the line and loses the inner quotes.)
python "$PSScriptRoot\vm.py" @args
exit $LASTEXITCODE
