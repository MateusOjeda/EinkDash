# EinkDash — monitor de tempo de tela do Windows

Registra qual programa está em primeiro plano a cada segundo; salva localmente a cada 15 segundos; envia ao Firebase Firestore a cada 5 minutos. Se a internet falhar, mantém as medições localmente para tentar de novo.

## Instalação (Windows 10/11)

1. Instale o Python 3.10+.
2. Abra o PowerShell dentro desta pasta e execute: `py -m pip install -r requirements.txt`.
3. Copie `.env.example` para `.env` e preencha:
   - `EINKDASH_USER_UID`: UID do usuário da autenticação Firebase (o mesmo usado no app Expo).
   - `EINKDASH_SERVICE_ACCOUNT`: caminho completo do JSON de uma conta de serviço do projeto Firebase.
   - `EINKDASH_DEVICE_ID`: nome estável do computador (sem espaços ou barras).
   - `EINKDASH_IDLE_SECONDS`: `0` para contar sempre que há aplicativo ativo, ou `300` para ignorar após 5 minutos sem entrada de mouse/teclado.
4. Execute: `py tracker.py`.
5. Após 5 minutos, verifique o Firestore. Dados ficam em `users/{uid}/screenTime/{AAAA-MM-DD}/devices/{deviceId}`.

O arquivo local de recuperação fica em `%LOCALAPPDATA%\EinkDashScreenTime\usage.json`.

### Exemplo no Firestore

`users/SEU_UID/screenTime/2026-10-29/devices/pc-principal`

```json
{
  "platform": "windows",
  "deviceId": "pc-principal",
  "date": "2026-10-29",
  "apps": { "chrome.exe": 1200, "code.exe": 3600 },
  "totalSeconds": 4800,
  "updatedAt": "timestamp do servidor"
}
```

## Execução automática

Depois de testar no PowerShell, abra **Agendador de Tarefas** do Windows, crie uma tarefa **Ao fazer logon** (usuário atual) e selecione **Iniciar um programa**. Como programa, indique o caminho do `pythonw.exe` da sua instalação Python; no campo argumentos, informe o caminho completo para `tracker.py`; em **Iniciar em**, coloque o caminho da pasta do projeto. Configure para **não iniciar nova instância se já estiver em execução**. Para depurar, use `python.exe` no lugar de `pythonw.exe`, porque o segundo esconde o console.

## Limitações e segurança

- Mede o aplicativo ativo, não cada aba ou site do navegador.
- Não mede processos trabalhando em segundo plano.
- `EINKDASH_IDLE_SECONDS=0` também pode contar tempo ausente do PC quando o Windows ainda expõe uma janela ativa; valor maior que zero pode subcontar vídeos/leitura.
- Esta versão ignora nomes típicos da tela de bloqueio, mas não implementa detecção completa de bloqueio, tela apagada ou suspensão.
- O Firebase Admin SDK **ignora as Security Rules do Firestore**. Esta solução usa conta de serviço apenas para prototipagem pessoal em um PC sob seu controle. **Não compartilhe a chave, não a suba ao GitHub nem a inclua em software distribuído.** Para distribuir a aplicação ou fortalecer a segurança, troque por login Firebase Auth com regras por usuário ou por uma API/Cloud Function de escrita autenticada.
- O estado local acumula dados por dia. Não execute duas instâncias simultaneamente; o Agendador deve impedir múltiplas instâncias.
- Um envio gera **uma gravação por dia modificado e dispositivo**, sobrescrevendo o resumo com os valores completos. Portanto, retentativas não somam o tempo duas vezes.
