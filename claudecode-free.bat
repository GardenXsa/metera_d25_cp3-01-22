@echo off
setlocal

REM ====== НАСТРОЙКИ OMNIROUTE ======

REM 1. Вставь сюда свой ключ OmniRoute
REM (Официальный SDK Anthropic использует ANTHROPIC_API_KEY)
set "ANTHROPIC_API_KEY=sk-69c3c436e2c9f412-9cc808-fbf4d128"

REM 2. Вставь сюда Anthropic-compatible endpoint OmniRoute
REM Пример: https://api.omniroute.ai/v1 или http://localhost:20128/v1
set "ANTHROPIC_BASE_URL=http://localhost:1080/v1"

REM 3. Опционально: если gateway не любит experimental betas (например, prompt caching)
set "CLAUDE_CODE_DISABLE_EXPERIMENTAL_BETAS=1"

REM 4. ПОДМЕНА МОДЕЛИ: укажи имя модели, которое ждет твой gateway
set "MY_CUSTOM_MODEL=kr/claude-sonnet-4.5"


echo =====================================================
echo Starting Claude Code via OmniRoute...
echo Endpoint: %ANTHROPIC_BASE_URL%
echo Model:    %MY_CUSTOM_MODEL%
echo =====================================================
echo.

REM Запускаем Claude Code и принудительно передаем ему выбранную модель
call claude --model %MY_CUSTOM_MODEL%

endlocal
pause