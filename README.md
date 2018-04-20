# 4d-plugin-scard
basic PC/SC implementation (read ``IDm``, ``PMm``) 

### Platform

| carbon | cocoa | win32 | win64 |
|:------:|:-----:|:---------:|:---------:|
|<img src="https://cloud.githubusercontent.com/assets/1725068/22371562/1b091f0a-e4db-11e6-8458-8653954a7cce.png" width="24" height="24" />|<img src="https://cloud.githubusercontent.com/assets/1725068/22371562/1b091f0a-e4db-11e6-8458-8653954a7cce.png" width="24" height="24" />|<img src="https://cloud.githubusercontent.com/assets/1725068/22371562/1b091f0a-e4db-11e6-8458-8653954a7cce.png" width="24" height="24" />|<img src="https://cloud.githubusercontent.com/assets/1725068/22371562/1b091f0a-e4db-11e6-8458-8653954a7cce.png" width="24" height="24" />|

### Version

<img src="https://cloud.githubusercontent.com/assets/1725068/18940649/21945000-8645-11e6-86ed-4a0f800e5a73.png" width="32" height="32" /> <img src="https://cloud.githubusercontent.com/assets/1725068/18940648/2192ddba-8645-11e6-864d-6d5692d55717.png" width="32" height="32" />

### About

WindowsのPC/SC API（``SCardEstablishContext``, ``SCardListReaders``, ``SCardGetStatusChange``, ``SCardConnect``, ``SCardTransmit``, ``SCardDisconnect``, ``SCardReleaseContext``）をコールしてスマートカードから``Idm``と``PMm``を取得するプラグインです。[``RC-S380``](https://www.sony.co.jp/Products/felica/consumer/products/RC-S380.html)で検証しました。

macOSでは，[``pcsc-lite``](https://pcsclite.alioth.debian.org/pcsclite.html)のシステム実装（``PCSC Framework``）をコールしています。プリインストールされた[``CCID Driver``](https://github.com/acshk/acsccid)のカードリーダー（Advanced Card Systems Ltd.，あるいは[``ACR1251CL-NTTCom``](https://www.ntt.com/business/services/application/authentication/jpki/download7.html)のような同等品）であれば，動くかもしれません（未検証）。
