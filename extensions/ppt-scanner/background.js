chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request.action === "capture_success") {
    const targetUrl = "https://selmtoe.github.io/Tetris_Simulator";
    
    chrome.tabs.create({ url: targetUrl }, (newTab) => {
      chrome.tabs.onUpdated.addListener(function listener(tabId, info) {
        if (tabId === newTab.id && info.status === 'complete') {
          chrome.tabs.onUpdated.removeListener(listener);
          chrome.scripting.executeScript({
            target: { tabId: tabId },
            world: 'MAIN',
            func: (data) => {
              let attempts = 0;
              const maxAttempts = 20;
              
              const trySend = () => {
                if (window.receiveExtensionImage) {
                  window.receiveExtensionImage(data);
                  console.log("画像データを転送しました");
                } else {
                  attempts++;
                  if (attempts < maxAttempts) {
                    console.log(`転送先関数が見つかりません リトライ中... (${attempts}/${maxAttempts})`);
                    setTimeout(trySend, 500);
                  } else {
                    console.error("エラー: window.receiveExtensionImage が見つかりませんでした");
                    alert("画像転送に失敗しました。シミュレータが正しく読み込まれていない可能性があります。");
                  }
                }
              };
              
              trySend();
            },
            args: [request.imageData]
          });
        }
      });
    });
  }
});

chrome.action.onClicked.addListener((tab) => {
  if (tab.id) {
    chrome.tabs.sendMessage(tab.id, { action: "capture_video_frame" });
  }
});
