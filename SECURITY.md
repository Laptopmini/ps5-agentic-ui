# Security

## Reporting a vulnerability

Please report it privately through GitHub: on the repository's **Security** tab, choose **Report a vulnerability**. Don't open a public issue for it. You should get a reply within a week.

## What's in scope

The parts of this project that listen on a network or handle secrets:

- **The agent server** (`runtime/serve.ts`, `server/index.ts`) and **the Channels adapter** (`channel/adapter.ts`). Both listen on loopback by default and refuse any other address without an access code of 16 or more characters, compared in constant time.
- **Key setup on the console** (`renderer/src/pairing.cpp`, `renderer/assets/pair.html`). While the setup screen is open the app serves a page on port 8791. The key is encrypted in the browser for the console's one-time public key (NaCl box) and accepted only with the six-digit code shown on the TV; five wrong tries rotate the code and the key pair.
- **Secret handles** in the agent runtime: text typed into a password keyboard must never reach the model or the logs.
- **The approval gate**: a tool that asks for approval must never run without the user's answer.

## Things to know before you deploy it

- **A model key stored on the console is readable by other homebrew on that console.** It's kept in the app's folder with owner-only permissions, which a jailbroken console doesn't enforce against other payloads. Use a key made for this purpose, with a spending limit where the provider offers one.
- **The LAN is trusted only as far as the access code.** Traffic between the console and a server or channel on your network is plain HTTP; the access code keeps other devices from using the agent, not from reading traffic. Don't expose these ports to the internet.
- **The agent server logs conversations** to `server/logs/` on the computer running it.

## Supported versions

The latest 1.x release.
