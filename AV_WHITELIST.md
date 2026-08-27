# Submitting a false positive report

AmethystTool injects a DLL into Steam and installs in-process hooks via Microsoft Detours. That is the same technique class antivirus heuristics use to catch real injectors, so a flag on an unsigned build is an expected false positive, not a sign anything is wrong with your build (see [README → Antivirus and SmartScreen](README.md#antivirus-and-smartscreen)).

If you want a specific vendor to stop flagging the file, submit it directly to that vendor. Nobody else can do this for you — each vendor ties the whitelist decision to the exact file hash you send them, and re-flags on the next rebuild until you (or someone) resubmits. This document is the checklist and links; it does not submit anything on its own.

## Before you submit, gather this

- **The exact file** you built (`AmethystTool.dll`, `dwmapi.dll`, or `xinput1_4.dll` — submit each one that was flagged, separately).
- **SHA-256 of the file:**
  ```powershell
  Get-FileHash AmethystTool.dll -Algorithm SHA256
  ```
- **The detection name** your antivirus reported (e.g. `HEUR:Trojan.Win32.Generic`, `Trojan:Win32/Wacatac`). Find it in the antivirus's quarantine/history log — vendors ask for this to route the sample to the right analyst queue.
- **A short, factual description** — see the template below. Do not editorialize or argue the tool's purpose beyond what's true; reviewers see hundreds of these and a plain technical description gets through faster than a defensive one.
- **A link to this repository** (or the specific release/commit) so the reviewer can see the source if they choose to.

## Description template

Adapt this to each vendor's submission form:

> This is a self-built/open-source Windows DLL (AmethystTool, a fork of OpenSteamTools) that injects into the Steam client process and installs inline hooks via Microsoft Detours (github.com/microsoft/Detours) to modify Steam client behavior. It is flagged because DLL injection + inline hooking is a heuristic your product uses for real malware, and this legitimate use case triggers the same signal. Source: `<repo URL>`. SHA-256: `<hash>`.

## Vendor submission links

| Vendor | Submission URL | Notes |
|---|---|---|
| Microsoft Defender | https://www.microsoft.com/en-us/wdsi/filesubmission | Requires a Microsoft account. Track status at https://www.microsoft.com/en-us/wdsi/submissionhistory |
| Kaspersky | https://opentip.kaspersky.com/ (lookup) → https://virusdesk.kaspersky.com/ (submit) | OpenTIP lets you check the hash first without uploading; VirusDesk is the actual submission/appeal form |
| ESET | https://www.eset.com/int/support/false-positive/ | |
| Avast / AVG | https://www.avast.com/false-positive-file-form | Shared submission form for both brands |
| Bitdefender | https://www.bitdefender.com/consumer/support/answer/29358/ | Submit via email per the instructions on that page |
| Malwarebytes | https://www.malwarebytes.com/support/malwarebytes-false-positive | |
| Norton / Symantec | https://submit.norton.com/false_positive.jsp | |
| McAfee | https://www.mcafee.com/enterprise/en-us/threat-center/threat-intelligence-report/false-positive.html | |

(This list is not exhaustive — if your vendor isn't here, search `"<vendor name>" false positive submission` from their own support site.)

## After submitting

- Vendors typically respond in 1–5 business days; some (Kaspersky, ESET) show a case/ticket number you can check later.
- A whitelist decision is scoped to the **exact hash submitted**. Every new build (even with no source changes, since PDB/timestamp/build-path bytes differ) has a new hash and may need resubmission — this is a real, unavoidable cost of an unsigned, frequently-rebuilt binary. Code signing (see the README section) does not remove this cost by itself, but a *consistent signing identity* across builds is what some vendors use to build reputation faster on resubmission.
- If a vendor asks clarifying questions, answer factually — pointing them at the Detours GitHub project and this repository's source is the fastest way to resolve ambiguity about what the code does.
