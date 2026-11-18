# rt5650-buddy

[![build](https://github.com/itsbeko/rt5650-buddy/actions/workflows/build.yml/badge.svg)](https://github.com/itsbeko/rt5650-buddy/actions/workflows/build.yml)

Pilote Windows (KMDF) minimal pour le codec audio **Realtek RT5650** de l'**Acer Chromebase 24** (carte Google *BUDDY*, Broadwell), afin d'avoir le son sur les haut-parleurs internes sous Windows 10/11.

## Comment ça marche

L'audio de cette machine passe par deux éléments :

| Élément | Rôle | Pilote |
|---|---|---|
| DSP Intel SST (Broadwell, SSP0) | Transport I2S des échantillons | [CoolStar SST](https://coolstar.org/chromebook/windows-install.html) (`csaudiosstcatpt`), en I2S *provider*, 2 canaux, MCLK 24 MHz |
| Codec RT5650 (I2C, ACPI `10EC5650`, adresse `0x1A`) | Conversion numérique/analogique et ampli | **ce pilote** |

`rt5650buddy` ne fait pas de transport audio. Il configure seulement le codec par I2C à chaque passage en D0 :

1. vérification de l'identifiant (`0x6419`) ;
2. reset et séquences d'initialisation ;
3. polarisation (bias) ;
4. horloges : PLL1 alimentée par MCLK 24 MHz, sortie ≈ 24,576 MHz, utilisée comme SYSCLK ;
5. interface I2S1 en esclave, 24 bits, 48 kHz ;
6. chemin DAC → mixeur haut-parleurs → ampli classe D.

Les valeurs de registres viennent du pilote Linux [`sound/soc/codecs/rt5645.c`](https://github.com/torvalds/linux/blob/master/sound/soc/codecs/rt5645.c), qui gère aussi le RT5650, et de la machine [`bdw-rt5650.c`](https://github.com/torvalds/linux/blob/master/sound/soc/intel/boards/bdw-rt5650.c).

## État

- ✅ Haut-parleurs internes
- ❌ Prise casque, micro, détection jack
- ❌ Réglage du volume côté codec (volume fixe, ~0 dB ; le volume Windows agit en amont)

## Compilation

### Par la CI (recommandé)

Chaque push lance [GitHub Actions](.github/workflows/build.yml). Le workflow compile le pilote, le signe avec un certificat de test généré à la volée et publie un artefact `rt5650buddy` qui contient `rt5650buddy.sys`, `.inf`, `.cat` et `pont-test.cer`.

### En local

Il faut Visual Studio 2022, le Windows SDK et le WDK.

```powershell
msbuild rt5650buddy.vcxproj /p:Configuration=Release /p:Platform=x64
```

## Installation

> ⚠️ Le pilote est signé avec un **certificat de test**, il faut donc activer le mode test de Windows. Ce code tourne en mode noyau : tu l'utilises à tes risques.

1. Installe d'abord le pilote CoolStar SST et vérifie qu'il démarre.
2. Active le mode test, puis redémarre :
   ```powershell
   bcdedit /set testsigning on
   ```
3. Importe `pont-test.cer` dans **Autorités de certification racines de confiance** et dans **Éditeurs approuvés** (magasin *Ordinateur local*).
4. Installe le pilote :
   ```powershell
   pnputil /add-driver rt5650buddy.inf /install
   ```

## Diagnostic

Après le démarrage, le pilote enregistre son résultat dans la clé `Device Parameters` du périphérique :

```
HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\10EC5650\<instance>\Device Parameters
```

| Valeur | Signification |
|---|---|
| `DeviceId` | ID lu dans le registre `0xFF` (attendu : `0x6419`) |
| `InitStatus` | `NTSTATUS` final (`0` = succès) |
| `InitStep` | Dernière étape d'init atteinte, pour situer un échec |

## Licence

[GPL-2.0](LICENSE), comme le pilote Linux dont proviennent les séquences d'initialisation.
