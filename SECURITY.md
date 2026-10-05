# Security

Report vulnerabilities privately: **Security** tab, then **Report a vulnerability**. Only the latest
release is supported.

Every release is built by GitHub Actions from the tagged source. Verify a download with the
[GitHub CLI](https://cli.github.com/):

```
gh attestation verify PingPeek.exe --repo Twiceyy/PingPeek
```
