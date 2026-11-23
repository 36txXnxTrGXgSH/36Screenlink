# Compile le lecteur WebAssembly à faible latence (player.cc -> player.js + player.wasm).
# Prérequis : l'Emscripten de Samsung (extensions Tizen), qui fournit les en-têtes samsung/wasm
# et le drapeau -s ENVIRONMENT_MAY_BE_TIZEN. Téléchargeable sur developer.samsung.com (WebAssembly).
#
# Deux correctifs nécessaires une fois dans ce SDK (version 1.39.4.7) :
#   - tools/system_libs.py : les bibliothèques lib_tizen_emss et lib_tizen_tv_wasm doivent être
#     compilées en C++17 (remplacer -std=c++14 par -std=c++17), sinon une lambda constexpr casse.
#   - Un fichier .emscripten valide pointant LLVM_ROOT, BINARYEN_ROOT et NODE_JS vers le SDK.
#
# Exemple : .\build-player.ps1 -Emsdk C:\emscripten-release-bundle\emsdk
param(
    # Dossier emsdk de l'Emscripten Samsung (contient fastcomp\emscripten\emcc.bat).
    [string]$Emsdk = $env:EMSDK
)
$ErrorActionPreference = 'Stop'
if (-not $Emsdk) { throw 'Indiquez le dossier emsdk de l''Emscripten Samsung : -Emsdk <dossier> (ou la variable EMSDK).' }
$emcc = Join-Path $Emsdk 'fastcomp\emscripten\emcc.bat'
if (-not (Test-Path $emcc)) { throw "Emscripten Samsung introuvable : $emcc" }
Set-Location $PSScriptRoot
# Mémoire FIXE de 64 Mo : le téléviseur lit les images par des vues sur cette mémoire, et une
# croissance la remplacerait, invalidant les vues en cours. Moteur maintenu vivant après main()
# pour les appels depuis JavaScript. AppendPacketAsync est autorisé sur le fil principal ; inutile
# de compiler en multi-thread (que ce téléviseur refuse de toute façon).
& $emcc player.cc -std=gnu++14 -O2 `
    -s ENVIRONMENT_MAY_BE_TIZEN -s TOTAL_MEMORY=67108864 -s NO_EXIT_RUNTIME=1 `
    -s "EXPORTED_FUNCTIONS=@exported.json" -s "EXTRA_EXPORTED_RUNTIME_METHODS=@runtime.json" `
    -o player.js
if ($LASTEXITCODE) { throw 'Compilation du lecteur WebAssembly' }
# Les bibliothèques Samsung gardent le chemin de leurs sources (messages d'assertion) : le dossier
# local du SDK ne doit pas partir dans le paquet publié. Remplacement à longueur égale, pour ne
# rien décaler dans la mémoire du module.
$bytes = [IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'player.wasm'))
$latin = [Text.Encoding]::GetEncoding(28591)
$text = $latin.GetString($bytes)
$sdkDir = (Resolve-Path $Emsdk).Path.TrimEnd('\')
foreach ($prefix in @(($sdkDir + '\'), ($sdkDir.Replace('\', '/') + '/'))) {
    $text = $text.Replace($prefix, 'emsdk/'.PadRight($prefix.Length, '_'))
}
[IO.File]::WriteAllBytes((Join-Path $PSScriptRoot 'player.wasm'), $latin.GetBytes($text))
if ($text.Contains($sdkDir)) { throw 'Un chemin local reste dans player.wasm' }
Write-Host "Lecteur compilé : player.js + player.wasm"
