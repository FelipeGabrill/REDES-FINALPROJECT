# NetBoX - aplicação User em C++17

Implementação do cliente NetBoX organizado em módulos. O Directory Server (DS) é
fornecido pela cadeira; esta pasta contém apenas a aplicação User e o protocolo de
transferência entre pares usado por esta implementação.

## Compilar e executar

```sh
make
./user -m 5000
./user -m 5000 -n 193.136.138.142 -p 59000
```

`-m` é obrigatório e define a porta TCP do servidor local de pares. `-n` define o
IPv4 do DS e `-p` a porta do DS. Os valores por omissão são os indicados no código
original do projeto; confirme-os com a cadeira antes da demonstração.

O programa serve pedidos TCP enquanto a interface de comandos está ativa. Os ficheiros
publicados e recebidos são procurados/criados no diretório de trabalho atual.

## Comandos

```text
login UID password
publish filename label
remove filename
list
versions filename
where filename
get filename peerIPv4 peerPort [destination]
logout
unregister
exit
```

`where` consulta as versões no DS e mostra os pares online/offline. `get` transfere
diretamente de um par cujo IPv4 e porta TCP sejam conhecidos. Pode indicar outro nome
de destino; a aplicação nunca sobrescreve um ficheiro existente.

## Cobertura do PDF

| Requisito | Estado nesta versão |
| --- | --- |
| Arranque com `-m`, `-n` e `-p` | Implementado, com validação de IPv4 e portas |
| Login, logout, unregister por UDP | Implementado com validação de campos e apresentação de estados |
| Publicação e remoção por UDP | Implementado; publicação verifica ficheiro local, tipo e limite de 10 MB |
| Lista de recursos por UDP | Implementado; valida nomes e limite de 50 entradas |
| Versões por TCP | Implementado; valida e apresenta UID, tamanho, label, data e disponibilidade |
| Servidor TCP de pares | Implementado; aceita `GET filename` e envia o ficheiro em blocos |
| Download TCP direto entre pares | Implementado através do comando `get` e do protocolo de pares descrito abaixo |
| Localização automática (`where`) | Parcial: o protocolo de Phase II não especifica uma mensagem DS para localização nem inclui IP/porta na resposta `RVR` |

### Limite da especificação de localização

O texto introdutório do PDF descreve `where` como pedido de localização, mas a
especificação de protocolo da Phase II não define essa mensagem nem a resposta com
endereço IP/porta. A resposta `RVR` de `versions` só define UID, tamanho, label, hora
de publicação e disponibilidade. Para evitar enviar ao DS uma mensagem inventada que
ele pode rejeitar, `where` apresenta as versões e informa essa limitação. É necessário
receber a especificação de localização da fase seguinte para automatizar a descoberta.

## Protocolo TCP entre pares nesta implementação

Este protocolo não é especificado no PDF da Phase II. É uma extensão local para tornar
funcionais o servidor de pares e o download descritos na introdução:

1. O cliente envia `GET filename\n`.
2. O servidor responde `OK Fsize\n` e envia exatamente `Fsize` bytes, ou `ERR\n`.
3. Os nomes seguem os limites do PDF. Tamanho máximo: 10.000.000 bytes.

O destino é criado sem sobrescrever ficheiros. Se a ligação falhar a meio, o ficheiro
parcial criado por essa tentativa é removido.

## Limites validados

- UID: 6 dígitos decimais.
- Password: 8 caracteres ASCII alfanuméricos.
- Filename: até 24 caracteres, com extensão de 3 caracteres alfanuméricos.
- Label: 1 a 20 letras ASCII, dígitos, `-` ou `_`.
- Tamanho de ficheiro: 0 a 10.000.000 bytes.
- `list`: até 50 nomes únicos, conforme resposta do DS.

Cada interação com o DS é sequencial; uma resposta UDP pode demorar até 10 segundos
antes de ser reportada como falhada.
