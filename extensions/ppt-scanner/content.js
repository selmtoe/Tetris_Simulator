(() => {
  if (window === window.top) {
    const notifyPage = () => {
      window.postMessage({ type: "PPT_EXTENSION_DETECTED" }, "*");
    };
    notifyPage();
    setTimeout(notifyPage, 1000);

    window.addEventListener("message", (event) => {
      if (event.data && event.data.type === "PPT_TRIGGER_SCAN") {
        captureVideo();
        document.querySelectorAll('iframe').forEach(iframe => {
          iframe.contentWindow.postMessage({ type: "PPT_CAPTURE_IFRAME" }, "*");
        });
      }
    });
  }

  window.addEventListener("message", (event) => {
    if (event.data && event.data.type === "PPT_CAPTURE_IFRAME") {
      captureVideo();
    }
  });

  chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
    if (request.action === "capture_video_frame") {
      captureVideo();
      if (window === window.top) {
        document.querySelectorAll('iframe').forEach(iframe => {
          iframe.contentWindow.postMessage({ type: "PPT_CAPTURE_IFRAME" }, "*");
        });
      }
    }
  });

  function captureVideo() {
    const video = document.querySelector('video');
    if (!video || video.readyState < 2) {
      return;
    }

    try {
      const canvas = document.createElement('canvas');
      canvas.width = video.videoWidth;
      canvas.height = video.videoHeight;
      const ctx = canvas.getContext('2d');
      ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
      const dataUrl = canvas.toDataURL('image/jpeg', 0.9);
      chrome.runtime.sendMessage({
        action: "capture_success",
        imageData: dataUrl
      });
      console.log("PPT Scanner: Capture sent.");
    } catch (e) {
      console.error("PPT Scanner: Capture failed.", e);
    }
  }
})();
