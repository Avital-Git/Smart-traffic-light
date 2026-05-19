# Deployment Keys

This folder contains the frozen delivery auth material for the target environment.

Files:
- `emergency_keys.json` - HMAC secrets for authenticated emergency vehicles.
- `neighbor_message_auth.json` - shared HMAC key used to sign inter-junction summaries.
- `api_keys.production.json` - production API keys for operator access.
- `.env.production.example` (project root) - production environment template that points to the auth files.

Recommended deployment steps:
1. Copy these files to a protected config directory on the target host.
2. Restrict read access to the service account only.
3. Set `TRAFFIC_API_KEYS_FILE` and, if needed, `TRAFFIC_EMERGENCY_KEYS_FILE` to the deployed paths.
4. Rotate the secrets after first production deployment if the repo leaves your machine.
