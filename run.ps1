# Launch IndiaRoute AI backend and frontend server via WSL2
Write-Host "==========================================" -ForegroundColor Cyan
Write-Host " Starting IndiaRoute AI on http://localhost:8082 " -ForegroundColor Green
Write-Host "==========================================" -ForegroundColor Cyan

wsl -d Ubuntu -e bash -c "cd /mnt/c/Users/arige/Downloads/IndiaRouteAI-full/IndiaRouteAI && ./scripts/run.sh"
