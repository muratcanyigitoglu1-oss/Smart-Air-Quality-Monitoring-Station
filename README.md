# Smart Air Quality Monitoring Station

## Proje Özeti
Bu proje, iç mekan hava kalitesini (CO2, duman, VOC) takip eden, otomatik havalandırmayı tetikleyen ve Bluetooth üzerinden mobil uyarı gönderen taşınabilir bir sağlık izleme istasyonudur.

## Kullanılan Teknolojiler & Bileşenler
- **Mikrodenetleyici:** MSP430G2553
- **Sensörler:** MQ-135 (Gaz), MQ-7 (Gaz), DHT22 (Sıcaklık + Nem)
- **Görselleştirme:** Graphic LCD (OLED)
- **Haberleşme:** HC-05 Bluetooth Modülü
- **Kontrol:** Röle Modülü (Fan Kontrolü)

## Teknik Detaylar
- **Periferaller:** ADC10 (Gaz sensörleri), Timer_A (DHT22 timing), USCI_B0 (I2C LCD), USCI_A0 (Bluetooth).
- **İşleyiş:** Sensör verileri okunarak hava kalitesi indeksi hesaplanır. Emoji tabanlı görselleştirme ile (Yeşil/İyi, Kırmızı/Tehlike) anlık durum bilgisi OLED ekrana yansıtılır.
- **Otomasyon:** Eşik değer aşıldığında fan otomatik olarak devreye girer ve Bluetooth üzerinden push bildirimi gönderilir.

## Özellikler
- Gerçek zamanlı hava kalitesi izleme.
- Emoji tabanlı görsel arayüz.
- Otomatik havalandırma kontrolü.
- Kablosuz Bluetooth bildirim sistemi.
