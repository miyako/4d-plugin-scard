# 4d-plugin-scard
basic PC/SC implementation (read ``IDm``, ``PMm``) 

### Platform

| carbon | cocoa | win32 | win64 |
|:------:|:-----:|:---------:|:---------:|
|||<img src="https://cloud.githubusercontent.com/assets/1725068/22371562/1b091f0a-e4db-11e6-8458-8653954a7cce.png" width="24" height="24" />|<img src="https://cloud.githubusercontent.com/assets/1725068/22371562/1b091f0a-e4db-11e6-8458-8653954a7cce.png" width="24" height="24" />|

### Version

<img src="https://cloud.githubusercontent.com/assets/1725068/18940649/21945000-8645-11e6-86ed-4a0f800e5a73.png" width="32" height="32" /> <img src="https://cloud.githubusercontent.com/assets/1725068/18940648/2192ddba-8645-11e6-864d-6d5692d55717.png" width="32" height="32" />

### About

WindowsのPC/SC API（``SCardEstablishContext``, ``SCardListReaders``, ``SCardGetStatusChange``, ``SCardConnect``, ``SCardTransmit``, ``SCardDisconnect``, ``SCardReleaseContext``）をコールしてスマートカードから``Idm``と``PMm``を取得するプラグインです。[``RC-S380``](https://www.sony.co.jp/Products/felica/consumer/products/RC-S380.html)専用です（内部的にFeliCa独自定義拡張APDUをコールしているため）。

## Syntax

```
SCARD READER LIST (readers)
```

Parameter|Type|Description
------------|------------|----
readers|ARRAY TEXT|カードリーダーの名称

```
info:=SCARD Get info (reader;mode;protocols;timeout;error)
```

Parameter|Type|Description
------------|------------|----
reader|TEXT|カードリーダーの名称
mode|LONGINT|[共有モード](https://msdn.microsoft.com/en-us/library/windows/desktop/aa379473(v=vs.85).aspx): ``SCARD_SHARE_SHARED``または``SCARD_SHARE_EXCLUSIVE``
protocols|LONGINT|[プロトコル](https://msdn.microsoft.com/en-us/library/windows/desktop/aa379473(v=vs.85).aspx): ``SCARD_PROTOCOL_T0``と``SCARD_PROTOCOL_T1``の組み合わせ
timeout|LONGINT|制御を返すまでのタイムアウト（ミリ秒）
error|LONGINT|エラーコード（``0``=成功）
info|TEXT|取得した情報（``JSON``）

* ``info``オブジェクト

``IDm``: ``string`` カードUID（16進数）    
``PMm``: ``string`` カードATS-HB/INF/PMm（16進数）  
``state``: ``number``   [リーダーの状態](https://msdn.microsoft.com/en-us/library/windows/desktop/aa379808(v=vs.85).aspx)  
``type``: ``string`` カード種別（16進数）   

* FeliCa独自定義（拡張）APDU

カードUIDの取得: ``FF CA 00 00 00``  
カードATS-HB(ISO14443-4A)/INF(ISO14443B)/PMm(FeliCa)の取得: ``FF CA 01 00 00``  
カード識別IDの取得: ``FF CA F0 00 00``  
カード名称の取得: ``FF CA F1 00 00`` 
カード種別の取得: ``FF CA F3 00 00`` 

* カードUID

ISO14443A: Cascade Level 1/2/3 の UID（4/7/10 bytes） 
ISO14443B: PUPI（4 bytes） 
PicoPass(iCLASS): SN（8 bytes）  
NFC Type 1 Tag: UID（7 bytes） 
FeliCa: IDm（8 bytes） 

* カード種別

``01``:ISO14443A  
``02``:ISO14443B  
``03``:PicoPassB  
``04``:FeliCa  
``05``:NFC Type 1 Tag    
``06``:Mifare Emulation Card    
``07``:ISO14443-4A  
``08``:ISO14443-4B  
``09``:TypeA NFC-DEP ターゲット 
``0A``:FeliCa NFC-DEP ターゲット 
