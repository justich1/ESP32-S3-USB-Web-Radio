ORIScore radio v0.3.8

Hlavní zdrojový kód Android aplikace:
  app/src/main/java/cz/oris/mobileaudio/

Android resources a manifest:
  app/src/main/res/
  app/src/main/AndroidManifest.xml

Novinky v této verzi:
- detailní 10pásmový ekvalizér pro nový firmware,
- maximální výstupní gain a křivka hlasitosti,
- EQ preamp a automatická ochranná rezerva,
- zvukové presety a bezpečný výchozí gain.

Build konfigurace:
- applicationId: cz.oris.mobileaudio
- minSdk: 26
- targetSdk: 36
- versionCode: 12
- versionName: 0.3.8

Aplikace očekává firmware s poli audioEqBands, audioEqPreampDb,
audioEqAutoHeadroom, audioOutputGainDb a audioVolumeCurve v /api/config.json.

Podpisový keystore není součástí zdrojového archivu. Pro aktualizace používej
samostatně uložený ORIScore-radio.keystore se stejným aliasem a hesly jako dříve.
