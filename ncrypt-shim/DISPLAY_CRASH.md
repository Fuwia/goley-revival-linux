# Çözünürlük değiştirme çökmesi — 10 Kasım 2026

## Doğrulanan kök neden

`WM_MOUSEWHEEL` için üretilen x86 thunk'ın uzunluğu **18 bayt**. Eski kod
diziyi 19 bayt olarak tanımlıyor, `E9 rel32` operandını `+14` yerine `+15`
konumuna yazıyor ve göreli adresi `+19` üzerinden hesaplıyordu.

```text
Thunk başlangıcı:         0x00d36b00
E9 komutu:               +13
rel32 operandı:          +14..+17
Sonraki komut adresi:     0x00d36b12
Beklenen hedef:          0x00d38b09
Eski operand baytları:   00 f6 1f 00
Gerçek eski hedef:       0x00d36b12 + 0x001ff600 = 0x00f36112
Doğru operand baytları:  f7 1f 00 00
```

Eski yama ve yalnızca tanılama eklenmiş DLL ile oyun açıldı. Gerçek ayarlar
menüsünden yapılan 16 Reset'in tamamı `S_OK` döndü. Ardından gönderilen **tek
bir fare tekerleği olayı** oyunu çökertti. VEH kaydı hesaplanan adresi doğruladı:

```text
exception tid code eip esp ebp eax ebx ecx edx esi edi access addr module
00000510 c0000005 00f36112 0026f824 0026f9e8 00000780 02f81c10
02f87758 0026f858 00000000 00000000 00000001 00000780 00400000
```

Bu kayıt, yakalanan çökmenin D3D9 Reset hatası veya birkaç Reset sonrasında
biriken kaynak kaybı değil, tekerlek olayındaki hatalı kontrol aktarımı olduğunu
gösterir. Çözünürlük listesinde gezinirken oluşması, hatanın çözünürlük
değişimine bağlanmasını açıklıyor.

## Düzeltme

- Thunk dizilerinin uzunluğu derleyici tarafından çıkarılıyor. Tekerlek
  atlamasının operand konumu ve sonraki komut adresi `sizeof(thunk2)` üzerinden
  hesaplanıyor; bağımsız `19` / `15` sabitleri kaldırıldı.
- Fare thunk'ı C/Win32 yardımcılarına geçerken `ECX` ve `EDX` yazmaçlarını
  koruyor. Ek iki push nedeniyle fare X/Y argümanlarının stack ofsetleri de
  düzeltildi. Dış çağrı hâlâ `ret 8`, iç yardımcı hâlâ stdcall.
- Fare girişinin, tekerlek devam noktasının ve yeniden kullanılan kod alanının
  tamamı yazmadan önce doğrulanıyor. Bir hook veya farklı istemci bu alanları
  değiştirmişse birbirine bağlı giriş yamaları kurulmaz.
- CRT gerektirmeyen Win32/VEH tanılaması `/tmp/goley_display.log` dosyasına
  yazıyor. İsteğe bağlı `GOLEY_DISPLAY_TRACE=1`, gerçek D3D9 Reset parametreleri
  ve HRESULT sonuçlarını kaydeder. Reset sonucu ve hata kurtarma akışı korunur.

## Diğer şüpheler

Kurulu `revival_client.dll` içinde eski notlardaki RVA `0x1700` / `0x19c0`
bu ekran hook'larına karşılık gelmiyor. İncelenen sürümde `0x00d324a9` ve
`0x00d37440` noktalarına kurulan tam ekran dönüşüm hook'ları bulundu.
`0x00d375b0`, `0x009aca70`, `0x009ab240` mutlak adreslerine doğrudan referans
bulunmadı. Bu, dolaylı D3D hook'larının yokluğunun kanıtı değildir; fakat
yakalanan çökme bu olasılığı gerektirmeden birebir açıklanıyor.

Özgün fare fonksiyonunun mode 2 yolu `ECX`'i kendisi de değiştiriyor; incelenen
WndProc çağrıları GFx'e geçmeden önce gerekli yazmaçları yeniden kuruyor.
Dolayısıyla ECX/EDX koruması ayrıca sağlamlaştırmadır; yakalanan çökmenin
kanıtlanmış nedeni olarak sunulmamalıdır.

`g_enumRealCount` ve şablon modu önbelleği paylaşılan durum içeriyor. Yakalanan
çökme, bu enumerator'a ihtiyaç duymayan tekerlek yolunda tekrarlandı. Enum
davranışı ve Reset/pencere güncelleme politikası bu düzeltmede değiştirilmedi.

Render yüksekliğinin asgari değerle sınırlandırılması motorun kendi
`ApplyDisplaySettings` akışında da var. Testlerde listeden 800×600 seçimi
1280×768 gerçek backbuffer üretti. Bu mevcut davranışın düzeltilmesi bu
çökme yamasının kapsamına alınmadı.

## Doğrulama

Yerel Clang/LLD araçlarıyla `cargo build --release` çalıştırılarak x86/x64
DLL'ler üretildi. Bu yerel derlemenin içe aktarımları yalnızca
`bcrypt.dll`, `kernel32.dll` ve `user32.dll`; ek CRT bağımlılığı yok.

[Makine kodu testleri](tests/README.md), derlenmiş DLL'nin gerçek kurulum ve
yardımcı kodunu çalıştırıyor. Yedi test geçti; bunların içinde 1.000 ardışık
çözünürlük/fare/tekerlek çevrimi de var. Eski DLL, yazmaç koruma ve tekerlek
devam adresi testlerinde başarısız oluyor.

İlk canlı oturumda 30 çözünürlük değişimi, 6 Onay/yeniden açma çevrimi ve
1.120 tekerlek olayı gönderildi. Bütün Reset sonuçları `S_OK`, istisna ve
çökme sayısı sıfırdı. Son release derlemesiyle ikinci canlı oturumda 80
çözünürlük değişimi, 1.320 tekerlek olayı, aynı backbuffer boyutunda 20
tam ekran/tam pencere modu değişimi ve 36 Onay/yeniden açma çevrimi daha
tamamlandı. Bu son oturumdaki 101 Reset çağrısının (başlangıç dahil) tamamı
`S_OK` döndü; VEH istisnası ve çökme olmadı. İki oturum toplamı: **110
çözünürlük değişimi, 20 aynı boyutta mod değişimi, 42 Onay çevrimi ve 2.440
tekerlek olayı**. Canlı testler gerçek Wine/DXVK/XWayland oturumunda
yapıldı. Wine'ın ayrı ses aygıtı başlatma takılması debugger ile `mmdevapi`
yoluna ayrıldığı için yalnızca test süreçlerinde PulseAudio bağlantısı
kapatıldı; kalıcı ses, Wine veya başlatıcı ayarı değiştirilmedi.

Canlı testte kullanılan yerel release başlatıcısının içine gömülen ve kurulu öneğe atomik olarak kopyalanan
DLL'lerin baytları birebir karşılaştırıldı:

```text
ncrypt32.dll
5554c9312fd4b8c005595f70eebbf1e89e45ff6696ed98c61a86d532c718d8db
ncrypt64.dll
d983f483f954206626e0f75173b21b35dfc8e0c635c2b0b0de97c0b239471b44
```

İncelenen dosyaların SHA-256 değerleri:

```text
BinaryTrU.exe
199c410ad9ced1f7ec8838e7f243366f96f63cde576f392c7de2965e9c3fad1f
revival_client.dll
51eeadc5b6dee16d62e9bc5fce6818c8fa5ed0d908048443213724e4428935c7
```

Tam disassembly, eski çökme kaydı, canlı Reset kayıtları ve stres testi çıktısı
yerel `/tmp/goley-display-investigation/` dizininde tutulur. Oyun dosyaları veya
hesap bilgileri depoya eklenmez.
