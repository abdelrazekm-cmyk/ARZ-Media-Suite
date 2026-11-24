# التوثيق الهندسي الشامل لحزمة `ARZ Media Suite v1.0.0`

![ARZ Logo](resources/arz_logo.jpg)

هذا المستند هو المرجع الهندسي الكامل لحزمة **`ARZ v1.0.0`** المستقلة (`Zero External Dependencies`) لبرنامجي **Adobe Premiere Pro 2027** و**Adobe Media Encoder**.

---

## 1. مكونات الحزمة المستقلة (`100% Self-Contained Build`)

| الملف | الدور الوظيفي |
| :--- | :--- |
| **`ARZ-PremierePlugin-v1.0.0-Windows-x64-Setup.exe`** | مُثبّت رسومي مستقل بملف واحد يحمل أيقونة وشعار **`ARZ`** ومبني بـ Static Runtime (`/MT`) — لا يحتاج العميل لتنزيل أي برامج أو مكتبات خارجية |
| **`ARZ.prm` + `ARZ_HP.prm`** | مستورد `ARZ` القياسي وعالي الأولوية (`priority = 10000`) لـ **59 امتداداً** مع واجهة `PrSDKAsyncImporter` |
| **`ARZ_Exporter.prm`** | مصدّر `ARZ` الأصلي داخل نافذة **`Ctrl+M`** بـ 7 صيغ رئيسية و12 Preset جاهزة و25 كوديك فيديو وصوت بعلامة **`[ARZ]`** |
| **`ARZDesktop.exe`** | مركز تحكم `ARZ` ثنائي اللغة (**عربي / إنجليزي**) بالثيم الداكن والذهبي ومراقب حي لاستهلاك الـ GPU والـ VRAM |
| **`ARZCore.dll` + مكتبات `FFmpeg 9.1+`** | المحرك المشترك المدمج داخل الحزمة (شامل `NVDEC/D3D11VA`, `NVENC AV1/HEVC`, `libdav1d`, `ProRes 4444/RAW`, `Opus`, `FLAC`) |

---

## 2. عدم الحاجة لأي متطلبات خارجية على أجهزة العملاء (`Zero-Dependency Guarantee`)

- تم ربط جميع ملفات الحزمة الثنائية (`ARZCore.dll`, `ARZ.prm`, `ARZ_HP.prm`, `ARZ_Exporter.prm`, `ARZDesktop.exe`, `Setup.exe`) بمكتبة الـ C++ الثابتة **`/MT` (`MultiThreaded`)**؛ فلا يحتاج جهاز العميل حتى إلى حزمة `VC++ Redistributable`.
- جميع مكتبات `FFmpeg 9.1+` الخمس مدمجة داخل ملف `Setup.exe` نفسه وتعتمد حصرياً على مكتبات نواة ويندوز القياسية (`kernel32.dll`, `user32.dll`, `d3d11.dll`, `dxgi.dll`, `bcrypt.dll`).

---

## 3. صيغ التصدير الـ 7 داخل نافذة `Ctrl+M` (`ARZ_Exporter.prm`)

1. **`ARZ - AV1 Hardware / SVT (.mp4 / .mkv)`**
2. **`ARZ - Matroska Universal (.mkv)`**
3. **`ARZ - WebM / VP9 + Alpha (.webm)`**
4. **`ARZ - ProRes / DNxHR / CineForm (.mov)`**
5. **`ARZ - FFV1 Archival Lossless (.mkv)`**
6. **`ARZ - Animated GIF / WebP / APNG (.gif)`**
7. **`ARZ - Audio Master Opus / FLAC / WAV (.opus)`**
