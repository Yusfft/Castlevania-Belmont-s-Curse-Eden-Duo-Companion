# Castlevania: Belmont's Curse – Eden Duo harita companion (test sürümü 0.1.0)

Thor'un alt ekranında oyunun haritasını gösteren Eden Duo paketi. Yalnızca şu sürümle çalışır:

| | |
|---|---|
| Oyun | Castlevania: Belmont's Curse, `0100BA4026E94000` |
| Güncelleme | 1.1.0 (build `0F1169AA7D0FA452…`) |
| Eden Duo | 1.3.x (runtime 16 ve üstü) |

Paket oyundan hiçbir şey taşımaz. Odalar, oyun çalışırken senin kendi romfs'indeki
`data-world_biome_*` dosyalarından okunur ve modül tarafından çizilir.

## Bu sürüm ne yapıyor?

- **MAP sayfası:** O an bulunduğun bölgenin (biome) haritası. Oyuncunun bulunduğu chunk'ta beyaz-kırmızı bir kare var.
  - `ZOOM`: Tüm bölge, geniş görünüm, yakın görünüm arasında geçiş yapar.
  - `FOG`: Keşfedilmemiş odaları soluk olarak gösterir veya gizler.
  - `- AREA` / `AREA +`: Başka bölgelerin haritasına bakmanı sağlar.
  - `FOLLOW`: Tekrar oyuncuyu takip etmeye döner.
- **DEBUG sayfası:** Modülün oyunun belleğinden okuduğu her değeri adım adım gösterir. Bu ilk sürüm,
  bellek zincirinin Thor'da doğru çalışıp çalışmadığını görmek için yapıldı.

## Paketi derlemek (GitHub Actions, bilgisayarına bir şey kurman gerekmez)

1. github.com'da ücretsiz bir hesap aç.
2. Sağ üstteki **+** menüsünden **New repository** seç. Bir isim ver (örneğin `belmonts-curse-dsmod`).
   **Private** seçebilirsin. **Create repository** de.
3. Açılan sayfada **uploading an existing file** bağlantısına tıkla. Bu zip'ten çıkan klasörün
   **içindeki her şeyi** (`.github`, `native`, `package`, `tools`, `README.md`, `LICENSE`) sürükleyip
   bırak. **Commit changes** de.
   - `.github` klasörünün de yüklendiğinden emin ol. Derleme talimatı onun içinde.
4. **Actions** sekmesine geç. Derleme kendiliğinden başlar. Başlamazsa **build** iş akışını aç ve
   **Run workflow** de. Yaklaşık 5 dakika sürer.
5. Yeşil tik çıkınca çalışmayı aç. En altta **Artifacts** altındaki `BelmontsCurseMap-0.1.0` dosyasını indir.
   İnen zip'in içinde `0100BA4026E94000.dsmod.zip` var. Onu çıkar, **adını değiştirme**.

## Thor'a kurmak

1. `0100BA4026E94000.dsmod.zip` dosyasını Thor'a kopyala.
2. Eden Duo'da oyuna uzun bas, **Add-ons** bölümüne gir ve **Install** de. **Dual screen mods** seç ve dosyayı göster.
3. Oyunu başlat ve kayıt dosyanı yükle. Alt ekranda harita sayfası açılır.

## Bana ne göndermelisin?

Oyunda bir odadayken **DEBUG** sayfasının fotoğrafını çek. İkinci bir fotoğrafı da yandaki odaya
geçince çek. Özellikle şu satırlar önemli: `STAGE`, `STATUS`, `CHUNK X Y`, `CHUNK MODE`,
`ROOM UID`, `FOG OK N MATCH`, `FOG HERE` ve `WORLD BIOMES ROOMS`. Bunlarla şunları doğrulayacağım:

- Oyuncu karesinin doğru chunk'ta olup olmadığını,
- Keşif (fog) değerlerinin anlamını,
- Odaların hangi anahtarla eşleştiğini.

## Geliştirme (bilgisayarda test)

```
cmake -S native -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/bc_live_test                                     # bellek okuyucu testi (sahte bellek)
build/bc_world_tool <romfs veya bundle klasörü> out    # haritaları PPM olarak çizer
build/bc_host_sim build/0100BA4026E94000.so <romfs> out # modülü Eden gibi yükleyip çalıştırır
python3 tools/gen_manifest.py                          # package/dualscreen/manifest.json'u üretir
```

Lisans: GPL-3.0-or-later. `native/third_party/eden` içindeki ABI başlıkları ve
`tools/build_dualscreen_package.py`, Eden Duo ve eden-duo-companions projelerinden (GPL-3.0) olduğu
gibi alındı.
