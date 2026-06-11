/* FTDI FT2232 USB Host Class Driver
 *
 * 아키텍처:
 *   - USB1 (EHCI HS) + USB0 (KHCI FS) 각각 독립 태스크
 *   - 각 포트: usbh_init → usbh_enable → FT2232 감지 → UART 설정 → 데이터 수신 루프
 *
 * FTDI 보드레이트 38400 8N1:
 *   FT2232D(PID=0x6001): 3MHz 기준 → wValue=0x404F, wIndex=0x0000
 *   FT2232H(PID=0x6010): 12MHz 기준 → wValue=0x0139, wIndex=0x0002
 *
 * 벌크 IN 패킷: 앞 2바이트 = FTDI 모뎀 상태 → 제거 후 콜백
 */

#include "ftdi_usb.h"

#include <zephyr/kernel.h>
#include <zephyr/usb/usbh.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "usbh_ch9.h"
#include "usbh_device.h"

LOG_MODULE_REGISTER(ftdi_usb, LOG_LEVEL_INF);

/* ── USB HOST 컨트롤러 선언 ──────────────────────────────────────────── */
USBH_CONTROLLER_DEFINE(s_usbh_usb1, DEVICE_DT_GET(DT_NODELABEL(usb1)));
USBH_CONTROLLER_DEFINE(s_usbh_usb0, DEVICE_DT_GET(DT_NODELABEL(usb0)));

/* ── FTDI VID/PID ────────────────────────────────────────────────────── */
#define FTDI_VID           0x0403U
#define FTDI_PID_FT2232D   0x6001U
#define FTDI_PID_FT2232H   0x6010U

/* ── FTDI 제어 요청 ──────────────────────────────────────────────────── */
#define FTDI_BMREQTYPE       0x40U
#define FTDI_REQ_RESET       0x00U
#define FTDI_REQ_SET_FLOW    0x02U
#define FTDI_REQ_SET_BAUD    0x03U
#define FTDI_REQ_SET_DATA    0x04U
#define FTDI_DATA_8N1        0x0008U
#define FTDI_INTERFACE_A     0x0000U

/* 38400 baud: FT2232D(3MHz 기준) / FT2232H(12MHz 기준) */
#define FTDI_BAUD_2232D_VALUE   0x404FU
#define FTDI_BAUD_2232D_INDEX   0x0000U
#define FTDI_BAUD_2232H_VALUE   0x0139U
#define FTDI_BAUD_2232H_INDEX   0x0002U

/* ── 벌크 수신 ───────────────────────────────────────────────────────── */
#define BULK_IN_EP    0x81U   /* FTDI Channel A Bulk IN */
#define BULK_BUF_SIZE 64U     /* FT2232D Full-Speed 최대 패킷 크기 */

/* ── 포트별 컨텍스트 ─────────────────────────────────────────────────── */
#define FTDI_STACK_SIZE 4096

K_THREAD_STACK_DEFINE(s_stack_usb1, FTDI_STACK_SIZE);
K_THREAD_STACK_DEFINE(s_stack_usb0, FTDI_STACK_SIZE);

struct ftdi_port_ctx {
	ftdi_port_t          id;
	struct usbh_context  *usbh;
	k_thread_stack_t    *stack;
	struct k_thread      tid;
	struct k_sem         rx_done;
	uint8_t              rx_data[BULK_BUF_SIZE];
	size_t               rx_len;
	int                  rx_err;
	bool                 connected;
	ftdi_rx_cb_t         rx_cb;
	void                *rx_user;
};

static struct ftdi_port_ctx s_ports[FTDI_PORT_COUNT] = {
	[FTDI_PORT_USB1] = {
		.id    = FTDI_PORT_USB1,
		.usbh  = &s_usbh_usb1,
		.stack = s_stack_usb1,
	},
	[FTDI_PORT_USB0] = {
		.id    = FTDI_PORT_USB0,
		.usbh  = &s_usbh_usb0,
		.stack = s_stack_usb0,
	},
};

/* ── 벌크 수신 완료 콜백 ──────────────────────────────────────────────── */
static int bulk_rx_callback(struct usb_device *udev, struct uhc_transfer *xfer)
{
	struct ftdi_port_ctx *ctx = (struct ftdi_port_ctx *)xfer->priv;

	ctx->rx_err = xfer->err;
	ctx->rx_len = 0;

	if (!ctx->rx_err && xfer->buf && xfer->buf->len > 2) {
		size_t data_len = xfer->buf->len - 2;

		if (data_len > BULK_BUF_SIZE) {
			data_len = BULK_BUF_SIZE;
		}
		memcpy(ctx->rx_data, xfer->buf->data + 2, data_len);
		ctx->rx_len = data_len;
	}

	k_sem_give(&ctx->rx_done);
	return 0;
}

/* ── FTDI 제어 전송 ───────────────────────────────────────────────────── */
static int ftdi_ctrl(struct usb_device *udev, uint8_t req,
		     uint16_t value, uint16_t index)
{
	return usbh_req_setup(udev, FTDI_BMREQTYPE, req, value, index, 0, NULL);
}

/* ── 1회 벌크 수신 ────────────────────────────────────────────────────── */
static int ftdi_bulk_read_once(struct ftdi_port_ctx *ctx,
			       struct usb_device *udev)
{
	const struct device *uhc_dev = ctx->usbh->dev;
	const unsigned pn = (ctx->id == FTDI_PORT_USB1) ? 1U : 0U;
	struct uhc_transfer *xfer;
	struct net_buf      *nbuf;
	int ret;

	xfer = uhc_xfer_alloc(uhc_dev, BULK_IN_EP, udev,
			      (void *)bulk_rx_callback, ctx);
	if (!xfer) {
		LOG_ERR("USB%u: xfer_alloc failed (ep=0x%02x, ep_in1_desc=%p)",
			pn, BULK_IN_EP, (void *)udev->ep_in[1].desc);
		return -ENOMEM;
	}

	nbuf = uhc_xfer_buf_alloc(uhc_dev, BULK_BUF_SIZE);
	if (!nbuf) {
		LOG_ERR("USB%u: buf_alloc failed (%u bytes)", pn, BULK_BUF_SIZE);
		uhc_xfer_free(uhc_dev, xfer);
		return -ENOMEM;
	}

	ret = uhc_xfer_buf_add(uhc_dev, xfer, nbuf);
	if (ret) {
		uhc_xfer_buf_free(uhc_dev, nbuf);
		uhc_xfer_free(uhc_dev, xfer);
		return ret;
	}

	ret = uhc_ep_enqueue(uhc_dev, xfer);
	if (ret) {
		LOG_ERR("USB%u: ep_enqueue failed: %d", pn, ret);
		/* nbuf already added to xfer — free buf then slab */
		uhc_xfer_buf_free(uhc_dev, nbuf);
		uhc_xfer_free(uhc_dev, xfer);
		return ret;
	}

	if (k_sem_take(&ctx->rx_done, K_MSEC(2000)) != 0) {
		/* uhc_ep_dequeue → _USB_HostKhciIoctl(kUSB_HostCancelTransfer) →
		 * 취소 콜백 동기 실행 → uhc_xfer_return (queued=0) → usbh_msgq 이벤트 등록.
		 * usbh_thread(높은 우선순위)가 이벤트 처리 → bulk_rx_callback → k_sem_give.
		 * 아래 k_sem_take로 이 시그널을 소비한 뒤 해제해야 슬랩 재할당 후
		 * 낡은 콜백이 새 xfer의 세마포어를 잘못 깨우는 use-after-free를 방지함. */
		uhc_ep_dequeue(uhc_dev, xfer);
		(void)k_sem_take(&ctx->rx_done, K_MSEC(200));
		uhc_xfer_buf_free(uhc_dev, nbuf);
		uhc_xfer_free(uhc_dev, xfer);
		return -ETIMEDOUT;
	}

	/* buf 반드시 해제: uhc_xfer_free는 net_buf를 해제하지 않음 */
	uhc_xfer_buf_free(uhc_dev, nbuf);
	uhc_xfer_free(uhc_dev, xfer);

	if (ctx->rx_err) {
		return ctx->rx_err;
	}

	if (ctx->rx_len > 0 && ctx->rx_cb) {
		ctx->rx_cb(ctx->id, ctx->rx_data, ctx->rx_len, ctx->rx_user);
	}

	return 0;
}

/* ── 포트별 FTDI 태스크 ───────────────────────────────────────────────── */
static void ftdi_task(void *p1, void *p2, void *p3)
{
	struct ftdi_port_ctx *ctx = (struct ftdi_port_ctx *)p1;
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int ret;

	ret = usbh_init(ctx->usbh);
	if (ret) {
		LOG_ERR("USB%u: usbh_init failed: %d",
			(ctx->id == FTDI_PORT_USB1) ? 1U : 0U, ret);
		return;
	}

	ret = usbh_enable(ctx->usbh);
	if (ret) {
		LOG_ERR("USB%u: usbh_enable failed: %d",
			(ctx->id == FTDI_PORT_USB1) ? 1U : 0U, ret);
		return;
	}

	LOG_INF("USB%u HOST ready. Waiting for FT2232...",
		(ctx->id == FTDI_PORT_USB1) ? 1U : 0U);

	while (1) {
		struct usb_device *udev = usbh_device_get_any(ctx->usbh);

		if (!udev || udev->state != USB_STATE_CONFIGURED) {
			ctx->connected = false;
			k_msleep(200);
			continue;
		}

		uint16_t vid = udev->dev_desc.idVendor;
		uint16_t pid = udev->dev_desc.idProduct;

		if (vid != FTDI_VID ||
		    (pid != FTDI_PID_FT2232D && pid != FTDI_PID_FT2232H)) {
			LOG_WRN("USB%u: Unknown device VID=0x%04X PID=0x%04X",
				(ctx->id == FTDI_PORT_USB1) ? 1U : 0U, vid, pid);
			k_msleep(1000);
			continue;
		}

		LOG_INF("USB%u: FT2232%s detected",
			(ctx->id == FTDI_PORT_USB1) ? 1U : 0U,
			(pid == FTDI_PID_FT2232H) ? "H" : "D");

		/* ftdi_configure 내부 LOG_INF 패치: 포트 번호 출력을 ctx로 */
		{
			uint16_t baud_val = (pid == FTDI_PID_FT2232H) ?
					FTDI_BAUD_2232H_VALUE : FTDI_BAUD_2232D_VALUE;
			uint16_t baud_idx = (pid == FTDI_PID_FT2232H) ?
					FTDI_BAUD_2232H_INDEX : FTDI_BAUD_2232D_INDEX;

			ret  = ftdi_ctrl(udev, FTDI_REQ_RESET, 0, FTDI_INTERFACE_A);
			k_msleep(50);
			ret |= ftdi_ctrl(udev, FTDI_REQ_SET_BAUD, baud_val, baud_idx);
			ret |= ftdi_ctrl(udev, FTDI_REQ_SET_DATA,
					 FTDI_DATA_8N1, FTDI_INTERFACE_A);
			ret |= ftdi_ctrl(udev, FTDI_REQ_SET_FLOW, 0, FTDI_INTERFACE_A);
		}

		if (ret) {
			LOG_ERR("USB%u: FTDI configure failed: %d",
				(ctx->id == FTDI_PORT_USB1) ? 1U : 0U, ret);
			k_msleep(2000);
			continue;
		}

		LOG_INF("USB%u: FT2232%s 38400 8N1 ready",
			(ctx->id == FTDI_PORT_USB1) ? 1U : 0U,
			(pid == FTDI_PID_FT2232H) ? "H" : "D");

		ctx->connected = true;

		while (udev->state == USB_STATE_CONFIGURED) {
			ret = ftdi_bulk_read_once(ctx, udev);
			if (ret == -ETIMEDOUT) {
				continue;
			}
			if (ret < 0) {
				LOG_WRN("USB%u: Bulk read error: %d",
					(ctx->id == FTDI_PORT_USB1) ? 1U : 0U,
					ret);
				break;
			}
		}

		ctx->connected = false;
		LOG_INF("USB%u: FT2232 disconnected.",
			(ctx->id == FTDI_PORT_USB1) ? 1U : 0U);
		k_msleep(500);
	}
}

/* ── 공개 API ─────────────────────────────────────────────────────────── */
int ftdi_usb_init(ftdi_port_t port, ftdi_rx_cb_t cb, void *user)
{
	if (port >= FTDI_PORT_COUNT || !cb) {
		return -EINVAL;
	}

	struct ftdi_port_ctx *ctx = &s_ports[port];

	ctx->rx_cb   = cb;
	ctx->rx_user = user;
	ctx->connected = false;

	k_sem_init(&ctx->rx_done, 0, 1);

	k_thread_create(&ctx->tid, ctx->stack, FTDI_STACK_SIZE,
			ftdi_task, ctx, NULL, NULL,
			K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
	k_thread_name_set(&ctx->tid,
			  (port == FTDI_PORT_USB1) ? "ftdi_usb1" : "ftdi_usb0");

	return 0;
}

bool ftdi_usb_connected(ftdi_port_t port)
{
	if (port >= FTDI_PORT_COUNT) {
		return false;
	}
	return s_ports[port].connected;
}
