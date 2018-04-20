# 4d-plugin-scard
basic PC/SC implementation (read ``IDm``, ``PMm``) 

### Platform

| carbon | cocoa | win32 | win64 |
|:------:|:-----:|:---------:|:---------:|
|<img src="https://cloud.githubusercontent.com/assets/1725068/22371562/1b091f0a-e4db-11e6-8458-8653954a7cce.png" width="24" height="24" />|<img src="https://cloud.githubusercontent.com/assets/1725068/22371562/1b091f0a-e4db-11e6-8458-8653954a7cce.png" width="24" height="24" />|<img src="https://cloud.githubusercontent.com/assets/1725068/22371562/1b091f0a-e4db-11e6-8458-8653954a7cce.png" width="24" height="24" />|<img src="https://cloud.githubusercontent.com/assets/1725068/22371562/1b091f0a-e4db-11e6-8458-8653954a7cce.png" width="24" height="24" />|

### Version

<img src="https://cloud.githubusercontent.com/assets/1725068/18940649/21945000-8645-11e6-86ed-4a0f800e5a73.png" width="32" height="32" /> <img src="https://cloud.githubusercontent.com/assets/1725068/18940648/2192ddba-8645-11e6-864d-6d5692d55717.png" width="32" height="32" />

### Releases

[1.0](https://github.com/miyako/4d-plugin-scard/releases/tag/1.0)

### About

WindowsのPC/SC API（``SCardEstablishContext``, ``SCardListReaders``, ``SCardGetStatusChange``, ``SCardConnect``, ``SCardTransmit``, ``SCardDisconnect``, ``SCardReleaseContext``）をコールしてスマートカードから``Idm``と``PMm``を取得するプラグインです。

検証には[``RC-S380``](https://www.sony.co.jp/Products/felica/consumer/products/RC-S380.html)を使用しました。カードリーダーの（PaSoRi）のドライバー（"[基本ソフトウェア](https://www.sony.co.jp/Products/felica/consumer/download/windows.html)"）をインストールしてください。

内部的にFeliCa独自の拡張APDUをコールしていますが，UIDやATSを取得するためのAPDUは，[PCSC 3 v2で定義されている](https://stackoverflow.com/questions/13051167/apdu-command-to-get-smart-card-uid/19789290#19789290)らしいので，他社のカードリーダーでも動くと思います（たぶん）。

[Advanced Card Systems Ltd.の仕様書](https://www.acs.com.hk/download-manual/4414/API-ACR1251U-1.08.pdf)

[SONYの仕様書](https://www.sony.co.jp/Products/felica/business/products/ICS-D004.html)

[MSDN](https://msdn.microsoft.com/ja-jp/library/windows/hardware/dn905498(v=vs.85).aspx)

[RC-S330](https://www.sony.co.jp/Products/felica/business/products/RC-S330.html)は，「PC/SCアクティベーター for Type B」をインストールしても，PC/SCのAPIでカードの情報を取得することはできません（ドキュメントより）。

macOSは，システムの[PCSC-Lite](http://pcsclite.alioth.debian.org)（PCSC Framework）を使用しています。システムに[ドライバー](https://github.com/acshk/acsccid)がインストールされているAdvanced Card Systems Ltd.のカードリーダー（[ACR1251CL-NTTCom](https://www.ntt.com/business/services/application/authentication/jpki/download7.html)など）であれば，動くかもしれません（未検証）。

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
timeout|LONGINT|タイムアウト（ミリ秒）カードが認識できない場合，この時間まで待機します。
error|LONGINT|エラーコード（``0``=成功）
info|TEXT|取得した情報（``JSON``）

* ``info``オブジェクト

``IDm``: ``string`` カードUID（16進数）    
``PMm``: ``string`` カードATS-HB/INF/PMm（16進数）  
``state``: ``number``   [リーダーの状態](https://msdn.microsoft.com/en-us/library/windows/desktop/aa379808(v=vs.85).aspx)  
``card``: ``string`` カード識別ID（16進数）  
``type``: ``string`` カード種別（16進数）  
``typeName``: ``string`` カード種別名称  
``name``: ``string`` カード名称（検証した範囲では常に空でした）  

* FeliCa独自定義（拡張）APDU

カードUIDの取得: ``FF CA 00 00 00``  
カードATS-HB(ISO14443-4A)/INF(ISO14443B)/PMm(FeliCa)の取得: ``FF CA 01 00 00``  
カード識別IDの取得: ``FF CA F0 00 00``  
カード名称の取得: ``FF CA F1 00 00``  
カード種別の取得: ``FF CA F3 00 00``  
カード種別名称の取得: ``FF CA F4 00 00``  

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

### Examples

<img width="290" alt="2018-04-20 13 10 26" src="https://user-images.githubusercontent.com/1725068/39030535-854c2088-449d-11e8-8400-76c1ca224356.png">

```
$Readers:=OBJECT Get pointer(Object named;"Readers")

$event:=Form event

Case of 
	: ($event=On Unload)
		
		SET TIMER(0)
		
	: ($event=On Load)
		
		SCARD READER LIST ($Readers->)
		
		If (Size of array($Readers->)#0)
			$Readers->:=1
			SET TIMER(50)
		End if 
		
	: ($event=On Timer)
		
		$reader:=$Readers->{$Readers->}
		$mode:=SCARD_SHARE_SHARED
		$protocols:=SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1
		$timeout:=50
		$error:=0
		
		$info:=SCARD Get info ($reader;$mode;$protocols;$timeout;$error)
		
		If ($error=0)
			
			$o:=JSON Parse($info)
			
			$state:=OB Get($o;"state";Is longint)
			
			If ($state & SCARD_STATE_PRESENT)=SCARD_STATE_PRESENT
				OBJECT Get pointer(Object named;"IDm")->:=OB Get($o;"IDm";Is text)
				OBJECT Get pointer(Object named;"PMm")->:=OB Get($o;"PMm";Is text)
				OBJECT Get pointer(Object named;"type")->:=OB Get($o;"type";Is text)
				OBJECT Get pointer(Object named;"typeName")->:=OB Get($o;"typeName";Is text)
				OBJECT Get pointer(Object named;"card")->:=OB Get($o;"card";Is text)
				OBJECT Get pointer(Object named;"cardName")->:=OB Get($o;"cardName";Is text)
			Else 
				OBJECT Get pointer(Object named;"IDm")->:=""
				OBJECT Get pointer(Object named;"PMm")->:=""
				OBJECT Get pointer(Object named;"type")->:=""
				OBJECT Get pointer(Object named;"typeName")->:=""
				OBJECT Get pointer(Object named;"card")->:=""
				OBJECT Get pointer(Object named;"cardName")->:=""
			End if 
      
		End if 
		
End case 
```
