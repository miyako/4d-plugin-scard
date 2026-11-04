![version](https://img.shields.io/badge/version-19%2B-5682DF)
![platform](https://img.shields.io/static/v1?label=platform&message=mac-intel%20|%20mac-arm%20|%20win-64&color=blue)
[![license](https://img.shields.io/github/license/miyako/4d-plugin-scard)](LICENSE)
![downloads](https://img.shields.io/github/downloads/miyako/4d-plugin-scard/total)

# 4d-plugin-scard

Reads contactless cards (FeliCa, ISO 14443 Type B) from a card reader and returns the card identifiers (`IDm`, `PMm`) and, optionally, the FeliCa usage history (service `090f`) as 4D objects. On macOS the plugin uses CryptoTokenKit (`TKSmartCardSlotManager`) for readers that have a system driver, and talks USB directly (libusb, libpafe) to Sony PaSoRi readers that don't. On Windows it uses the PC/SC API (`SCardConnect`, `SCardTransmit`).

Reading runs on a background thread: [SCARD Read tag](#scard-read-tag) starts it and returns a session `uuid` at once, and [SCARD Get status](#scard-get-status) polls the result.

> **Work in progress (4.0).** This branch merges [4d-plugin-scard-v0](https://github.com/miyako/4d-plugin-scard-v0), [-v2](https://github.com/miyako/4d-plugin-scard-v2) and [-v3](https://github.com/miyako/4d-plugin-scard-v3) into this repository. Its commands are those of scard-v3 3.1.x. The 1.x commands `SCARD READER LIST` and `SCARD Get info` are **not** in this build yet; use release [1.3.0](https://github.com/miyako/4d-plugin-scard/releases/tag/1.3.0) if you need them.

| Command | Returns | Purpose |
|---|---|---|
| [SCARD Get readers](#scard-get-readers) | Object | List the card readers the plugin can use |
| [SCARD Read tag](#scard-read-tag) | Object | Start reading a card on one reader, in the background |
| [SCARD Get status](#scard-get-status) | Object | Get the progress and result of a read |

**Platforms:** macOS 11.5 or later (Apple silicon and Intel), Windows 64-bit.

---

## Requirements & platform notes

- **macOS entitlement.** CryptoTokenKit and the PC/SC framework are only available to a code-signed app that carries the [`com.apple.security.smartcard`](https://developer.apple.com/documentation/bundleresources/entitlements/com_apple_security_smartcard?language=objc) entitlement. The entitlement must be in the **4D application's** signature, not the plugin's. When it is missing, [SCARD Get readers](#scard-get-readers) and [SCARD Read tag](#scard-read-tag) add a `warning` property to their result. To sign an app with entitlements, see [4d-class-build-application](https://github.com/miyako/4d-class-build-application) and [signing with a free account](with-free-account.md).
- **Sony PaSoRi on macOS.** There is no macOS driver for these readers, so the plugin opens them over USB itself and gives them its own names:

  | Reader | Name returned on macOS | Access |
  |---|---|---|
  | RC-S380 | `Sony FeliCa RC-S380` | libusb |
  | RC-S310 / RC-S320 / RC-S330 | `Sony FeliCa RC-S310` / `…RC-S320` / `…RC-S330` | libpafe + libusb |
  | RC-S300 | the name CryptoTokenKit reports | CryptoTokenKit |

  On Windows the vendor driver is used, and readers appear under their PC/SC names (the RC-S380 is `Sony FeliCa Port/PaSoRi 3.0 0`).
- **Tested combinations** (from the scard-v3 README):

  | Platform | Reader | IDm/PMm | Usage history (`090f`) |
  |---|---|---|---|
  | macOS | RC-S300 | OK | OK |
  | macOS | RC-S380 | OK | OK |
  | macOS | RC-S330 | OK | OK |
  | Windows | RC-S380 | OK | OK |
  | Windows | RC-S330 | not recognised | not recognised |

  An RC-S380 connected through a USB-C Digital AV Multiport adapter returns IDm/PMm but fails reading `090f`; a plain USB-C to USB adapter works.
- **Failure is reported in the result, never as a 4D error.** Check `success` (and `complete` for a read).
- All three commands are thread-safe and can be called from a preemptive process.

---

## SCARD Get readers

### Syntax

```4d
status:=SCARD Get readers(options)
```

| Parameter | Type | Description |
|---|---|---|
| `options` | Object | **On Windows**: `scope`, the PC/SC context scope (`0` user, the default; `1` terminal; `2` system). Other values are ignored. **On macOS**: ignored. |
| Result | Object | See below |

| Property | Type | Description |
|---|---|---|
| `success` | Boolean | `True` if the reader list could be obtained |
| `readers` | Collection | One object per reader |
| `readers[].slotName` | Text | Reader name; pass it to [SCARD Read tag](#scard-read-tag) |
| `warning` | Text | macOS only: the `com.apple.security.smartcard` entitlement is missing |
| `errorMessage` | Text | macOS only: CryptoTokenKit is not available |

### Description

**On macOS**, the list contains the CryptoTokenKit slots, followed by any Sony PaSoRi found on USB (by vendor and product ID) under the names in the table above. `success` is `True` when CryptoTokenKit is available, even if no reader is connected.

**On Windows**, the list comes from `SCardListReaders`. If the smart card service is not running, the plugin waits up to 5 seconds for it to start. When no reader is connected, `success` is `False`.

### Example

From the plugin's own test method (`TEST_w.4dm`):

```4d
$status:=SCARD Get readers 

If ($status.success)
	
	If (Is macOS:C1572)
		$readers:=$status.readers.query("slotName == :1";"Sony FeliCa RC-S3@")
	Else 
		$readers:=$status.readers
	End if 
```

---

## SCARD Read tag

### Syntax

```4d
status:=SCARD Read tag(params)
```

| Parameter | Type | Description |
|---|---|---|
| `params.slotName` | Text | Reader name from [SCARD Get readers](#scard-get-readers) |
| `params.timeout` | Integer | Seconds to wait for a card. Default `60`. Values outside 1–86400 are ignored (since this branch). See below for which readers use it. |
| `params.card` | Text | `"FeliCa"` (default), `"TypeB"` or `"TypeA"`. Only used by the RC-S380 on macOS. |
| `params.system` | Boolean | `True` to also read the FeliCa usage history (service `090f`). Default `False`. |
| Result | Object | See below |

| Property | Type | Description |
|---|---|---|
| `uuid` | Text | Session identifier; pass it to [SCARD Get status](#scard-get-status) |
| `warning` | Text | macOS only: the `com.apple.security.smartcard` entitlement is missing |

### Description

The command returns immediately; the read runs on a background thread. Poll [SCARD Get status](#scard-get-status) with the returned `uuid` until `complete` is `True`.

How the card is read depends on the reader:

- **macOS, RC-S380**: polls for a card of the `card` type until `timeout` seconds have passed. FeliCa returns `IDm` and `PMm`. Type B returns `nfcid`, `appdata`, `pinfo` and `cid` (from 4.0.1). Type A is not supported (the anti-collision sequence is not implemented), so the read runs until `timeout`.
- **macOS, RC-S310/320/330**: polls once for a FeliCa card (about 2 seconds); `timeout` and `card` are not used. With `system`, the history is read from system code `0003` or `80DE`.
- **macOS, other readers** (CryptoTokenKit): reads `IDm` with `FF CA 00 00 00` and `PMm` with `FF CA 01 00 00` from the card already on the reader. There is no wait: if no card is present, the read completes at once with `success` `False`. `timeout` and `card` are not used.
- **Windows** (PC/SC): waits up to `timeout` seconds for a card, then reads `IDm` and `PMm` with the same commands. `card` is not used.

With `system`, the result's `serviceData.data` holds the `090f` blocks, one 32-character hex string per 16-byte block. The number of blocks currently differs per reader (up to 18 on the RC-S380, 19 on CryptoTokenKit, 20 on Windows, all available blocks on the RC-S330).

### Example

From the plugin's own test method (`TEST_w.4dm`):

```4d
$params:=New object:C1471
$params.slotName:=$readers[0].slotName
$params.timeout:=3  //seconds; default=60
$params.card:="FeliCa"  //FeliCa,TypeB,TypeA
$params.system:=Bool:C1537($form.system)

$status:=SCARD Read tag ($params)

Repeat 
	
	$status:=SCARD Get status ($status.uuid)
	
	DELAY PROCESS:C323(Current process:C322;10)
	
Until ($status.complete)
```

---

## SCARD Get status

### Syntax

```4d
status:=SCARD Get status(uuid)
```

| Parameter | Type | Description |
|---|---|---|
| `uuid` | Text | Session identifier returned by [SCARD Read tag](#scard-read-tag) |
| Result | Object | See below |

| Property | Type | Description |
|---|---|---|
| `uuid` | Text | The session identifier |
| `complete` | Boolean | `True` once the read has finished, whether or not it succeeded |
| `success` | Boolean | `True` if at least one identifier was read |
| `slotName` | Text | Reader name; empty until `complete` is `True` |
| `IDm` | Text | Card UID (FeliCa IDm), lowercase hex. Absent if not read. |
| `PMm` | Text | FeliCa PMm (or ATS historical bytes), lowercase hex. Absent if not read. |
| `serviceData` | Object | Only with `system`: `code` (`"090f"`) and `data`, a collection of hex block strings |
| `nfcid` | Text | RC-S380 on macOS, Type B only: PUPI (4 bytes, hex). From 4.0.1. |
| `appdata` | Text | RC-S380 on macOS, Type B only: application data (4 bytes, hex). From 4.0.1. |
| `pinfo` | Text | RC-S380 on macOS, Type B only: protocol info (3 bytes, hex). From 4.0.1. |
| `cid` | Text | RC-S380 on macOS, Type B only: CID (1 byte, hex). From 4.0.1. |
| `errorMessage` | Text | Why the read failed: the reader could not be opened (macOS CryptoTokenKit), or an unexpected internal error (from 4.0.1) |

### Description

Once a call returns `complete` = `True`, the session is deleted. A later call with the same `uuid`, or with an unknown `uuid`, returns `complete` = `False` and `success` = `False`, and keeps doing so. Stop polling at the first completed result, and give your polling loop its own deadline.

`IDm` and `PMm` are up to 8 bytes (16 hex characters). Since this branch, a shorter UID is returned at its real length on macOS CryptoTokenKit (scard-v3 padded it with whatever bytes followed in memory).

### Example

From the plugin's own test method (`TEST_w.4dm`), handling the completed status in the form:

```4d
If ($status.success)
	If ($status.serviceData#Null:C1517)
		Form:C1466.serviceData:=$status.serviceData
		Form:C1466.info:=map_090f (Form:C1466.serviceData.data)
	End if 
	
	Form:C1466.IDm:=$status.IDm
Else 
	Form:C1466.IDm:=""
	Form:C1466.serviceData:=Null:C1517
	Form:C1466.info:=Null:C1517
End if 
```

The test database's `map_090f` and `parse_090f` methods decode the `090f` blocks of Japanese transit IC cards (date, balance, stations).

A polling loop with its own deadline:

```4d
$status:=SCARD Read tag(New object("slotName"; $slotName; "timeout"; 10))
$deadline:=Milliseconds+15000

Repeat 
	DELAY PROCESS(Current process; 10)
	$result:=SCARD Get status($status.uuid)
Until ($result.complete) | (Milliseconds>$deadline)
```

---

## Error handling & troubleshooting

- **`warning` in the result (macOS).** The 4D application is not signed with `com.apple.security.smartcard`; CryptoTokenKit readers may not be visible. Re-sign the app with the entitlement.
- **PaSoRi missing from the list on macOS.** The plugin finds it by USB vendor/product ID; check the USB connection and adapter, and that no other application holds the device.
- **Read never completes with `success`.** No card was presented within `timeout` (RC-S380, Windows), or no card was on the reader when the read started (macOS CryptoTokenKit), or the card type does not match `card` (RC-S380).
- **Polling never sees `complete`.** The `uuid` is wrong, or its completed result was already returned once. Keep only the first completed result.
- **Type B on the RC-S380 returns `success` with no identifiers.** Fixed in 4.0.1; 4.0.0 read them but did not return them.
- **`090f` read fails behind a USB-C AV adapter (RC-S380).** Use a plain USB-C to USB adapter.
- **Quitting 4D waits for running reads.** In 4.0.0 this could take up to each read's `timeout`. From 4.0.1, unfinished reads are cancelled on quit. They stop after the reader request in progress, which normally takes under 2 seconds; a USB poll on the RC-S380 can take up to 25 seconds if the reader stops responding.
- **An unexpected internal error** (out of memory, no thread available) is reported as `success` = `False` with an `errorMessage`, from 4.0.1. In 4.0.0 the command returned nothing.

---

## Quick reference

```4d
$readers:=SCARD Get readers(New object).readers
$status:=SCARD Read tag(New object("slotName"; $readers[0].slotName; "timeout"; 10; "system"; True))
Repeat 
	DELAY PROCESS(Current process; 10)
	$result:=SCARD Get status($status.uuid)
Until ($result.complete)
// $result.success, $result.IDm, $result.PMm, $result.serviceData.data
```

## References

* [PaSoRi RC-S380 specifics](https://tomosoft.jp/design/?p=5543)
* [Accessing FeliCa Lite over PC/SC in C](https://qiita.com/gpsnmeajp/items/d4810b175189609494ac)
* [Reading Suica history with WebUSB](https://www.kenichi-odo.com/articles/2020_10_11_read-suica-by-webusb)
* [nfcpy](https://github.com/nfcpy/nfcpy), whose RC-S380 and RC-S956 drivers document the Sony protocols
