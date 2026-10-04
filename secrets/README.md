# Secrets

Everything in this directory is ignored by git except this file and the
`*.example` templates. Each secret is one line in its own file:

| File | What | Where to get it |
|---|---|---|
| `muse_sdk_token` | Muse Gadget SDK token (`mgst_…`), required to pair | [gadgets.muse.ai › SDK tokens](https://gadgets.muse.ai/settings/sdk-tokens) |
| `elevenlabs_api_key` | ElevenLabs API key (`sk_…`), optional: spoken replies | [elevenlabs.io › API keys](https://elevenlabs.io/app/settings/api-keys), with the **Text to Speech** permission |
| `elevenlabs_voice_id` | ElevenLabs voice, optional (default: Sarah) | any voice ID in your ElevenLabs library |

```sh
cp secrets/muse_sdk_token.example secrets/muse_sdk_token   # then paste your token
chmod 600 secrets/*
```

`esp32/tools/muse/board.sh build <board>` copies them into that build's
generated `sdkconfig` through `esp32/tools/muse/secrets.py`. Environment
variables (`MUSE_SDK_TOKEN`, `ELEVENLABS_API_KEY`, `ELEVENLABS_VOICE_ID`) win
over the files, which suits CI.

Both secrets end up inside the firmware image. Treat a flashed board and its
`build-*/` directory as carrying them: never publish either. If a key leaks,
revoke it and build again.
