---
name: release
description: Release a new version of moss - bump the version, check the build, package the zip and winget manifests, publish the GitHub release, verify the download, and submit the winget update. Use when the user asks to release, ship, publish a version, cut v0.x.y, or update moss on winget.
---

# Release moss

The user asking for a release is the go-ahead for the push, the GitHub release and the winget pull request of the
version agreed in step 1, nothing else. Stop and ask when anything below doesn't go as described.

Never push `private-history` (the old history holds code that must stay private) and never commit
`docs/techniques.md`. Only `main` goes to `origin` (github.com/kevinmkchin/moss).

## 1. The version

Read `code/moss_version.h`. Agree the new version with the user (patch for fixes, minor for features), then set
`MOSS_VERSION_MAJOR/MINOR/PATCH` and `MOSS_VERSION` there.

## 2. Check it

- On `main`, nothing uncommitted but the version bump: `git status --short`.
- Build: `cmd //c "C:\\UnitySrc\\moss\\build.bat"`, no warnings.
- The full headless pass, once: `cd tests && sh run_all.sh`. Every line `same` (or differences you can account for
  and bless). Don't release with a failing set.

## 3. Commit and push

```
git commit -am "moss X.Y.Z" (with the attribution line)
git push origin main
```

## 4. Package

```
powershell -ExecutionPolicy Bypass -File tools/release/package.ps1
winget validate --manifest "dist\winget\manifests\n\NoclipSoftware\Moss\X.Y.Z"
cmd //c "C:\\UnitySrc\\moss\\build.bat"          # the packager left a release build in build\: the dev build back
```
It prints the zip (`dist\moss-X.Y.Z-win-x64.zip`) and its SHA256, which the manifests now carry. From here on don't
run the packager again: every run makes a zip with another hash. If you must, the zip uploaded has to be the new one.

## 5. The GitHub release

`gh auth status`: if the active account is `kevinmkchin`, release with it:
```
gh release create vX.Y.Z "dist/moss-X.Y.Z-win-x64.zip" --repo kevinmkchin/moss --target main --title "moss X.Y.Z" --notes "<what changed, short>"
```
Otherwise (`kevinmkchin-unity` can't write there) the user does it in the browser: github.com/kevinmkchin/moss/releases/new,
tag `vX.Y.Z` (created on publish, target `main`), title `moss X.Y.Z`, attach the zip with its name unchanged,
Publish. Give them those steps and wait.

## 6. Verify the download

```
curl -sL -o "$TEMP/moss-check.zip" -w "%{http_code} %{size_download}\n" https://github.com/kevinmkchin/moss/releases/download/vX.Y.Z/moss-X.Y.Z-win-x64.zip
sha256sum "$TEMP/moss-check.zip"
```
200, and the hash equal to `InstallerSha256` in `dist\winget\...\NoclipSoftware.Moss.installer.yaml`. If not, the
uploaded zip isn't the packaged one: stop.

## 7. winget

The token must not be typed on a command line or appear in the chat: the user stores one once with
`wingetcreate token --store` (a personal-account token with `public_repo`), then:
```
wingetcreate submit "dist\winget\manifests\n\NoclipSoftware\Moss\X.Y.Z"
```
It opens a pull request on microsoft/winget-pkgs and prints its link. If there's no stored token, ask the user to run
`! wingetcreate token --store` themselves. If the user pastes a token into the chat anyway, use it once and tell them
to revoke it.

## 8. Tell the user

The release link, the winget pull request link, and: bots validate within the hour, a moderator merges it (days for
a first package, faster for updates), then `winget upgrade moss` brings it. Leave the release's zip as it is from
now on.
