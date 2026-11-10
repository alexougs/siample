#!/bin/bash
# Installe Siample (VST3 + Audio Unit) pour l'utilisateur courant.
# Utilisation : ouvre le Terminal, tape « bash » suivi d'un espace,
# glisse ce fichier dans la fenêtre, puis appuie sur Entrée.
set -e
cd "$(dirname "$0")"

VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
AU_DIR="$HOME/Library/Audio/Plug-Ins/Components"

mkdir -p "$VST3_DIR" "$AU_DIR"

# Remplace une éventuelle ancienne version de Siample
rm -rf "$VST3_DIR/Siample.vst3" "$AU_DIR/Siample.component"
cp -R Siample.vst3 "$VST3_DIR/"
cp -R Siample.component "$AU_DIR/"

# macOS bloque les plugins téléchargés qui ne sont pas signés par Apple :
# on retire ce blocage, uniquement sur les deux fichiers de Siample.
xattr -cr "$VST3_DIR/Siample.vst3" "$AU_DIR/Siample.component"

# Force macOS à redécouvrir les Audio Units (pour Logic Pro)
killall -9 AudioComponentRegistrar 2>/dev/null || true

echo ""
echo "Siample est installé :"
echo "  $VST3_DIR/Siample.vst3"
echo "  $AU_DIR/Siample.component"
echo ""
echo "Relance FL Studio ou Logic Pro, puis lance une recherche de plugins."
