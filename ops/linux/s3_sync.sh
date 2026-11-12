#!/usr/bin/env bash
# ops/linux/s3_sync.sh -- mirror data/ to S3 every 5 minutes (altair-s3-sync.timer).
#
# Reads /etc/altair/s3.env (root 0600):
#   ALTAIR_S3_BUCKET=my-bucket          required
#   ALTAIR_S3_PREFIX=server             optional (default "server")
#   AWS_DEFAULT_REGION=ap-south-1       optional
#   AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY   only if the instance has no IAM role
#
# Never on the hot path:
#   - runs in system.slice, which altair-cpuset holds on the housekeeping cores
#     08:45-15:50; nice 19, idle I/O class
#   - in market hours: today's tapes are skipped (they are still being written,
#     and a growing file would be re-uploaded every run) and the upload is capped
#     at ALTAIR_S3_MBPS (default 8 MB/s) so the feed NIC never queues behind it
#   - after the session: everything, today's tapes included, at full speed
#   - one run at a time (flock); a run still going when the next is due is skipped
# Uploads only: nothing in S3 is ever deleted (no --delete).
# Never uploaded: session/token/credential/secret/.env files, wherever they are.
#
#   ops/linux/s3_sync.sh            # what it would send (aws s3 sync --dryrun)
#   ops/linux/s3_sync.sh --go
#   sudo ops/linux/s3_sync.sh --install-cli    # AWS CLI v2 into /usr/local (once)

set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
go=0
case "${1:-}" in
    --go) go=1 ;;
    --install-cli)
        [ "$(id -u)" = 0 ] || { echo "run it with sudo" >&2; exit 2; }
        command -v aws >/dev/null && { aws --version; exit 0; }
        t="$(mktemp -d)"; trap 'rm -rf "$t"' EXIT
        apt-get install -y -q unzip >/dev/null
        curl -fsSL "https://awscli.amazonaws.com/awscli-exe-linux-$(uname -m).zip" -o "$t/aws.zip"
        unzip -q "$t/aws.zip" -d "$t" && "$t/aws/install" --update
        aws --version; exit 0 ;;
    "") ;;
    *) echo "usage: s3_sync.sh [--go | --install-cli]" >&2; exit 2 ;;
esac

[ -r /etc/altair/s3.env ] && { set -a; . /etc/altair/s3.env; set +a; }
bucket="${ALTAIR_S3_BUCKET:-}"
[ -n "$bucket" ] || { echo "s3 sync off: no ALTAIR_S3_BUCKET in /etc/altair/s3.env"; exit 0; }
command -v aws >/dev/null || { echo "no aws CLI: sudo ops/linux/s3_sync.sh --install-cli" >&2; exit 2; }
prefix="${ALTAIR_S3_PREFIX:-server}"
export AWS_DEFAULT_REGION="${AWS_DEFAULT_REGION:-ap-south-1}"
export AWS_CONFIG_FILE="${XDG_RUNTIME_DIR:-/tmp}/altair-s3.config"

exec 9>"/tmp/altair-s3-sync.lock"
flock -n 9 || { echo "previous sync still running; skipped"; exit 0; }

today="$(TZ=Asia/Kolkata date +%F)"
args=(--only-show-errors --no-follow-symlinks
      --exclude '*session*' --exclude '*token*' --exclude '*credential*' --exclude '*secret*'
      --exclude '*.env' --exclude '*.lock' --exclude '*.tmp' --exclude '*.part')
if "$here/session_window.sh" 08:45 15:50; then
    mbps="${ALTAIR_S3_MBPS:-8}"
    args+=(--exclude "store/tapes/$today/*" --exclude "live/tapes/*")
    when="session (today's tapes held back, ${mbps} MB/s cap)"
else
    mbps=0; when="after hours (everything)"
fi
{ echo "[default]"; echo "s3 ="; echo "  max_concurrent_requests = 4"
  [ "$mbps" != 0 ] && echo "  max_bandwidth = ${mbps}MB/s"; } > "$AWS_CONFIG_FILE"
[ "$go" = 1 ] || args+=(--dryrun)

start=$(date +%s)
nice -n 19 ionice -c 3 aws s3 sync "$root/data/" "s3://$bucket/$prefix/data/" "${args[@]}"
echo "s3 sync to s3://$bucket/$prefix/data/: $when, $(( $(date +%s) - start )) s"
