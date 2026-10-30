# EinkDash Screen Time

Monitor de tempo de tela para **Windows 10/11**. Identifica o aplicativo em primeiro plano, registra o tempo de uso e envia os totais diários ao **Firebase Firestore**. Foi criado para alimentar os indicadores **PC** e **TV** do painel EinkDash.

## 1. Como funciona

- Verifica a janela em primeiro plano a cada **1 segundo**.
- **TV:** YouTube, Netflix e MPC-HC. Conta enquanto a janela estiver em primeiro plano, **mesmo sem interação** com mouse/teclado.
- **PC:** VS Code, Photoshop, Explorador, ChatGPT, demais sites do Chrome, Amazon Music, WhatsApp e outros programas (pelo nome do executável). Conta somente enquanto houver interação recente com mouse ou teclado; o limite padrão de inatividade é **60 segundos**.
- YouTube, Netflix e ChatGPT no Chrome são identificados **pelo título da aba**, não pela URL. Portanto, a identificação pode falhar ou classificar incorretamente algumas abas.
- Salva os dados localmente aproximadamente a cada **15 segundos**.
- A cada **5 minutos**, sincroniza com o Firestore **se houver mudanças**; se ficar inativo, pode não fazer uma nova gravação.
- Cada segundo contabilizado entra em **PC ou TV**, nunca nos dois. Não contabiliza aplicativos em segundo plano.

## 2. Arquivos do projeto

```text
ScreenTime/
├── tracker.py
├── usage_model.py
├── requirements.txt
├── .env.example
├── .env                  # criar localmente; NÃO compartilhar
└── README.md
```

O JSON da conta de serviço do Firebase deve ser guardado **fora do repositório**, em local privado.

## 3. Primeira instalação

### 3.1. Instalar Python

Instale o **Python 3.10 ou superior** para Windows e confirme no PowerShell:

```powershell
py --version
```

Descubra o caminho da instalação usada pelo comando `py`:

```powershell
py -c "import sys; print(sys.executable)"
```

Para execução **sem janela de terminal**, use o `pythonw.exe`, na mesma pasta do `python.exe`:

```powershell
py -c "import sys, os; print(os.path.join(os.path.dirname(sys.executable), 'pythonw.exe'))"
```

> Exemplo desta instalação: `C:\Program Files\Python313\pythonw.exe`. Confirme o caminho em cada PC; não presuma que será o mesmo.

### 3.2. Copiar os arquivos e instalar dependências

Coloque os arquivos do projeto numa pasta permanente, por exemplo:

```text
D:\Arquivos de Programas\ScreenTime
```

Abra o PowerShell nessa pasta e execute:

```powershell
py -m pip install -r requirements.txt
```

### 3.3. Configurar o Firebase

No **Firebase Console** do projeto EinkDash:

1. Entre em **Configurações do projeto → Contas de serviço**.
2. Selecione **Python** para ver o exemplo de SDK (a chave JSON é independente da linguagem).
3. Clique em **Gerar nova chave privada**.
4. Guarde o JSON em uma pasta privada, **fora do repositório**.

Na pasta do ScreenTime, copie `.env.example` para `.env` e configure:

```env
EINKDASH_SERVICE_ACCOUNT=C:\caminho\privado\firebase-service-account.json
EINKDASH_DEVICE_ID=pc-principal
EINKDASH_PC_IDLE_SECONDS=60
```

- `EINKDASH_SERVICE_ACCOUNT`: caminho **real** do JSON secreto do Firebase.
- `EINKDASH_DEVICE_ID`: identificador do computador no Firestore. O ESP32 está configurado para procurar **`pc-principal`**.
- `EINKDASH_PC_IDLE_SECONDS`: segundos sem mouse/teclado antes de pausar a categoria PC. **TV não pausa por inatividade**. Deve ser maior que zero.
- **Não** é necessário `EINKDASH_USER_UID` nem `EINKDASH_IDLE_SECONDS`: são opções de versões antigas, não usadas pelo código atual.

**Segurança:** a conta de serviço tem privilégios administrativos no projeto Firebase. Nunca inclua o arquivo JSON ou o `.env` no GitHub, em ZIPs distribuídos ou em executáveis compartilhados. Esta versão é apropriada para uso local, no seu computador.

## 4. Teste manual (antes de automatizar)

Com o PowerShell aberto na pasta do projeto:

```powershell
py tracker.py
```

Faça um teste alternando entre VS Code, Chrome/YouTube e MPC-HC. Os registros devem aparecer no terminal, por exemplo:

```text
09:10:00 | pc.vscode
09:11:00 | (sem atividade contabilizada)
09:12:00 | tv.youtube
09:15:00 | Enviando 1 dia(s)...
09:15:03 | Sincronização concluída
```

Verifique os dados no **Firestore**. Para encerrar o teste, use `Ctrl+C`.

**Não deixe o monitor manual aberto quando for iniciar o automático:** duas instâncias simultâneas podem prejudicar a contagem e sobrescrever dados.

## 5. Iniciar automaticamente, sem janela

Use o **Agendador de Tarefas** do Windows. Não é necessário transformar o script em `.exe`.

1. Pressione `Win + R`, digite `taskschd.msc` e abra o **Agendador de Tarefas**.
2. Escolha **Criar Tarefa** (não *Criar Tarefa Básica*).
3. Aba **Geral**:
   - Nome: **EinkDash Screen Time**.
   - Marque **Executar somente quando o usuário estiver conectado**. O monitor precisa acessar a janela em primeiro plano da sua sessão.
   - Deixe **Executar com privilégios mais altos** desmarcado.
4. Aba **Disparadores → Novo**: selecione **Ao fazer logon** do seu usuário e deixe habilitado.
5. Aba **Ações → Novo → Iniciar um programa**: preencha os campos **separadamente**:

   | Campo | Exemplo usado neste PC |
   |---|---|
   | **Programa/script** | `C:\Program Files\Python313\pythonw.exe` |
   | **Adicionar argumentos** | `"D:\Arquivos de Programas\ScreenTime\tracker.py"` |
   | **Iniciar em** | `D:\Arquivos de Programas\ScreenTime` |

   **Atenção às aspas e espaços:**
   - No campo **Programa/script**, use **Procurar...** e selecione `pythonw.exe` para evitar que o caminho `Program Files` seja separado incorretamente. O programa **não é** `C:\Program`.
   - No campo **Adicionar argumentos**, mantenha **aspas** em torno do caminho de `tracker.py`, pois ele contém espaços.
   - No campo **Iniciar em (opcional)**, informe **somente a pasta do projeto, sem aspas**, ainda que tenha espaços. **Não coloque `tracker.py` nesse campo.** Aspas aqui causaram o erro `2147942667`.
6. Aba **Condições**: desmarque exigências de computador ocioso ou energia da tomada, se estiverem habilitadas e não forem desejadas.
7. Aba **Configurações**:
   - Selecione **Não iniciar uma nova instância**, se a tarefa já estiver em execução.
   - Desative **Parar a tarefa se ela for executada por mais de 3 dias**, se aparecer marcada.
8. Salve a tarefa.

A tarefa inicia quando **você entra no Windows** e continua em segundo plano enquanto a sessão estiver aberta. Não monitora o computador desligado, suspenso ou depois de sair da sessão.

## 6. Confirmar que a tarefa funciona

No **PowerShell**, inicie a tarefa manualmente para testar (sem abrir `tracker.py` por outro terminal):

```powershell
Start-ScheduledTask -TaskName "EinkDash Screen Time"
Start-Sleep -Seconds 3
```

Confira o estado e o resultado:

```powershell
Get-ScheduledTask -TaskName "EinkDash Screen Time" |
  Select-Object TaskName, State

Get-ScheduledTaskInfo -TaskName "EinkDash Screen Time" |
  Select-Object LastRunTime, LastTaskResult
```

Procure o processo do tracker:

```powershell
Get-CimInstance Win32_Process |
  Where-Object { $_.Name -in @('python.exe','pythonw.exe') -and $_.CommandLine -like '*tracker.py*' } |
  Select-Object ProcessId, Name, CommandLine
```

O esperado é encontrar `pythonw.exe` executando `tracker.py` e a tarefa no estado **Running**. Isso confirma que o processo está ativo, mas **não garante que está sincronizando**.

O **teste definitivo** é usar um aplicativo da categoria PC por alguns minutos, esperar o próximo ciclo de sincronização e verificar se `totalSecondsPC` e `updatedAt` aumentaram no documento do dia no Firestore. Em seguida, reinicie o Windows, entre na conta e confirme que o processo voltou **sem execução manual**.

Para parar ou reiniciar a tarefa:

```powershell
Stop-ScheduledTask -TaskName "EinkDash Screen Time"
Start-ScheduledTask -TaskName "EinkDash Screen Time"
```

## 7. Solução de problemas

| Sintoma ou código | Causa provável / o que verificar |
|---|---|
| **Ready** no Agendador | A tarefa existe, mas **não está rodando**. Verifique `LastTaskResult`. |
| **2147942402** (`0x80070002`) | Arquivo não encontrado. Verifique o caminho de `pythonw.exe` no campo **Programa/script**. |
| **2147942667** (`0x8007010B`) | Diretório inválido. Verifique **Iniciar em**: pasta existente e **sem aspas**. |
| Tarefa roda e volta para **Ready** | O Python pode ter encerrado com erro. Teste `py tracker.py` na pasta do projeto para enxergar a mensagem. |
| Processo ativo, Firestore não muda | Confirme a chave JSON, conexão com internet, atividade recente no PC e intervalo de 5 minutos. Sem novos dados, o programa **não envia**. |
| PC para de contar | Comportamento esperado após `EINKDASH_PC_IDLE_SECONDS` sem mouse/teclado. |
| YouTube aparece como Chrome | Identificação baseada em **título de aba**, que nem sempre contém “YouTube”. |

Comandos para validar caminhos **antes** de salvar a tarefa:

```powershell
Test-Path 'C:\Program Files\Python313\pythonw.exe'
Test-Path 'D:\Arquivos de Programas\ScreenTime\tracker.py'
Test-Path 'D:\Arquivos de Programas\ScreenTime'
```

Todos devem retornar `True` (ajuste os caminhos se instalou em outra pasta).

Para examinar o que o Agendador realmente salvou:

```powershell
(Get-ScheduledTask -TaskName 'EinkDash Screen Time').Actions |
  Format-List Execute, Arguments, WorkingDirectory
```

## 8. Onde os dados ficam

**Firestore (resumo diário):**

```text
screenTime/{deviceId}/days/{AAAA-MM-DD}
```

Exemplo (`screenTime/pc-principal/days/2026-10-30`):

```json
{
  "date": "2026-10-30",
  "deviceId": "pc-principal",
  "platform": "windows",
  "tv": { "youtube": 3600, "mpc": 1200, "netflix": 0 },
  "pc": { "vscode": 5400, "chatgpt": 1200, "chromeOutros": 300 },
  "totalSecondsTV": 4800,
  "totalSecondsPC": 6900,
  "updatedAt": "timestamp do Firestore"
}
```

*Valores ilustrativos. O programa também grava outros campos para aplicativos conhecidos e executáveis utilizados.* Os totais são **segundos acumulados no dia** e são **substituídos** por totais atualizados a cada envio; não há dupla soma a cada sincronização.

**Estado local:**

```text
%LOCALAPPDATA%\EinkDashScreenTime\usage-v3.json
```

Esse arquivo permite retomar a contagem após reiniciar o programa e reenviar dados pendentes em caso de falha de rede. Não o apague enquanto houver dados ainda não enviados. **Não há migrações automáticas** de formatos antigos.

## 9. Checklist de nova instalação

- [ ] Python instalado e `py --version` funcionando.
- [ ] `requirements.txt` instalado.
- [ ] Projeto colocado em pasta permanente.
- [ ] `.env` criado com chave Firebase, `pc-principal` e limite de inatividade.
- [ ] Teste `py tracker.py` funcionando e enviando ao Firestore.
- [ ] Tarefa criada **Ao fazer logon**, no usuário correto.
- [ ] `pythonw.exe` selecionado corretamente.
- [ ] Argumento `tracker.py` **com aspas**; **Iniciar em sem aspas**.
- [ ] Tarefa em **Running** e processo `pythonw.exe` visível.
- [ ] Firestore atualizado e reinicialização do Windows testada.
