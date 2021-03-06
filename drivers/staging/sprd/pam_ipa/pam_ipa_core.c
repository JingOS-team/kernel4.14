#include <linux/sipa.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/dma-mapping.h>
#include <linux/skbuff.h>
#include <linux/regmap.h>
#include <linux/sipc.h>
#include <linux/mfd/syscon.h>
#include <linux/workqueue.h>
#include <linux/pm_runtime.h>
#include "pam_ipa_core.h"
#include "../sipa_delegate/sipa_delegate.h"

#define DRV_NAME "sprd-pam-ipa"

#define PAM_IPA_PCIE_RC_BASE_L					0x0
#define PAM_IPA_PCIE_RC_BASE_H					0x0

struct pam_ipa_cfg_tag *pam_ipa_cfg;

static const struct of_device_id pam_ipa_plat_drv_match[] = {
	{ .compatible = "sprd,pam-ipa", },
	{}
};

static void pam_ipa_prepare_suspend(struct device *dev);
static int pam_ipa_prepare_resume(struct device *dev);

static int pam_ipa_alloc_buf(struct pam_ipa_cfg_tag *cfg)
{
	cfg->dl_dma_addr = smem_alloc(SIPC_ID_MINIAP,
				      cfg->local_cfg.dl_fifo.fifo_depth *
				      PAM_AKB_BUF_SIZE);
	if (!cfg->dl_dma_addr)
		return -ENOMEM;

	cfg->ul_dma_addr = smem_alloc(SIPC_ID_MINIAP,
				      cfg->local_cfg.ul_fifo.fifo_depth *
				      PAM_AKB_BUF_SIZE);
	if (!cfg->ul_dma_addr) {
		smem_free(SIPC_ID_MINIAP, cfg->dl_dma_addr,
			  cfg->local_cfg.dl_fifo.fifo_depth *
			  PAM_AKB_BUF_SIZE);
		return -ENOMEM;
	}

	return 0;
}

static int pam_ipa_parse_dts_configuration(struct platform_device *pdev,
					   struct pam_ipa_cfg_tag *cfg)
{
	int ret;
	u32 reg_info[2];
	struct resource *resource;
	struct sipa_comm_fifo_params *recv_param =
		&cfg->pam_local_param.recv_param;
	struct sipa_comm_fifo_params *send_param =
		&cfg->pam_local_param.send_param;

	/* get IPA global register base  address */
	resource = platform_get_resource_byname(pdev,
						IORESOURCE_MEM,
						"pam-ipa-base");
	if (!resource) {
		pr_err("%s :get resource failed for glb-base!\n",
		       __func__);
		return -ENODEV;
	}
	cfg->reg_base = devm_ioremap_nocache(&pdev->dev,
					     resource->start,
					     resource_size(resource));
	memcpy(&cfg->pam_ipa_res, resource,
	       sizeof(struct resource));

	/* get enable register informations */
	cfg->enable_regmap = syscon_regmap_lookup_by_name(pdev->dev.of_node,
							  "enable");
	if (IS_ERR(cfg->enable_regmap))
		pr_warn("%s :get enable regmap fail!\n", __func__);

	ret = syscon_get_args_by_name(pdev->dev.of_node,
				      "enable", 2,
				      reg_info);
	if (ret < 0 || ret != 2)
		pr_warn("%s :get enable register info fail!\n", __func__);
	else {
		cfg->enable_reg = reg_info[0];
		cfg->enable_mask = reg_info[1];
	}

	of_property_read_u32(pdev->dev.of_node,
			     "sprd,cp-ul-intr-to-ap",
			     &recv_param->intr_to_ap);

	of_property_read_u32(pdev->dev.of_node,
			     "sprd,cp-ul-threshold",
			     &recv_param->tx_intr_threshold);

	of_property_read_u32(pdev->dev.of_node,
			     "sprd,cp-ul-timeout",
			     &recv_param->tx_intr_delay_us);

	of_property_read_u32(pdev->dev.of_node,
			     "sprd,cp-ul-flowctrl-mode",
			     &recv_param->flow_ctrl_cfg);

	of_property_read_u32(pdev->dev.of_node,
			     "sprd,cp-ul-enter-flowctrl-watermark",
			     &recv_param->tx_enter_flowctrl_watermark);

	of_property_read_u32(pdev->dev.of_node,
			     "sprd,cp-ul-exit-flowctrl-watermark",
			     &recv_param->tx_leave_flowctrl_watermark);

	of_property_read_u32(pdev->dev.of_node,
			     "sprd,cp-dl-intr-to-ap",
			     &send_param->intr_to_ap);

	of_property_read_u32(pdev->dev.of_node,
			     "sprd,cp-dl-threshold",
			     &send_param->tx_intr_threshold);

	of_property_read_u32(pdev->dev.of_node,
			     "sprd,cp-dl-timeout",
			     &send_param->tx_intr_delay_us);

	of_property_read_u32(pdev->dev.of_node,
			     "sprd,cp-dl-flowctrl-mode",
			     &send_param->flow_ctrl_cfg);

	of_property_read_u32(pdev->dev.of_node,
			     "sprd,cp-dl-enter-flowctrl-watermark",
			     &send_param->tx_enter_flowctrl_watermark);

	of_property_read_u32(pdev->dev.of_node,
			     "sprd,cp-dl-exit-flowctrl-watermark",
			     &send_param->tx_leave_flowctrl_watermark);

	return 0;
}

static int pam_ipa_connect_ipa(void)
{
	int ret;
	struct pam_ipa_cfg_tag *cfg = pam_ipa_cfg;
	u32 depth;

	ret = sipa_get_ep_info(SIPA_EP_VCP, &cfg->local_cfg);
	if (ret) {
		dev_err(&cfg->pdev->dev, "local ipa open fail\n");
		return ret;
	}

	ret = pam_ipa_alloc_buf(cfg);
	if (ret)
		return ret;

	depth = cfg->local_cfg.ul_fifo.fifo_depth;
	cfg->pam_local_param.send_param.flow_ctrl_irq_mode = 2;
	cfg->pam_local_param.recv_param.flow_ctrl_cfg = 1;
	cfg->pam_local_param.recv_param.tx_enter_flowctrl_watermark =
		depth - depth / 4;
	cfg->pam_local_param.recv_param.rx_enter_flowctrl_watermark =
		depth / 2;

	cfg->pam_local_param.send_param.data_ptr = cfg->dl_dma_addr;
	cfg->pam_local_param.send_param.data_ptr_cnt =
		cfg->local_cfg.dl_fifo.fifo_depth;
	cfg->pam_local_param.send_param.buf_size = PAM_AKB_BUF_SIZE;

	cfg->pam_local_param.recv_param.data_ptr = cfg->ul_dma_addr;
	cfg->pam_local_param.recv_param.data_ptr_cnt =
		cfg->local_cfg.ul_fifo.fifo_depth;
	cfg->pam_local_param.recv_param.buf_size = PAM_AKB_BUF_SIZE;

	cfg->pam_local_param.id = SIPA_EP_VCP;
	cfg->ready = true;

	return 0;
}

int pam_ipa_on_miniap_ready(struct sipa_to_pam_info *remote_cfg)
{
	struct pam_ipa_cfg_tag *cfg = pam_ipa_cfg;
	int ret;

	if (!cfg)
		return -EINVAL;

	memcpy(&cfg->remote_cfg, remote_cfg, sizeof(*remote_cfg));

	if (!cfg->ready) {
		ret = pam_ipa_connect_ipa();
		if (ret) {
			dev_err(&cfg->pdev->dev,
				"pam_ipa_connect_ipa fail:%d\n",
				ret);
			return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL(pam_ipa_on_miniap_ready);

static void pam_ipa_prepare_suspend(struct device *dev)
{
	int ret;
	struct pam_ipa_cfg_tag *cfg = dev_get_drvdata(dev);

	if (!(cfg->suspend_stage & PAM_IPA_REG_SUSPEND)) {
		sipa_disconnect(SIPA_EP_VCP, SIPA_DISCONNECT_START);
		cfg->hal_ops.resume(cfg->reg_base, false);
		cfg->suspend_stage |= PAM_IPA_REG_SUSPEND;
		sipa_disconnect(SIPA_EP_VCP, SIPA_DISCONNECT_END);
	}

	if (!(cfg->suspend_stage & PAM_IPA_EB_SUSPEND)) {
		pam_ipa_set_enabled(cfg, false);
		cfg->suspend_stage |= PAM_IPA_EB_SUSPEND;
	}

	if (!(cfg->suspend_stage & PAM_IPA_FORCE_SUSPEND)) {
		ret = pm_runtime_put_sync(&cfg->pdev->dev);
		if (ret < 0)
			dev_err(&cfg->pdev->dev,
				"pm runtime put err, ret = %d\n",
				ret);
		cfg->suspend_stage |= PAM_IPA_FORCE_SUSPEND;
	}
}

static int pam_ipa_prepare_resume(struct device *dev)
{
	int ret;
	struct pam_ipa_cfg_tag *cfg = dev_get_drvdata(dev);

	if (cfg->suspend_stage & PAM_IPA_FORCE_SUSPEND) {
		ret = pm_runtime_get_sync(&cfg->pdev->dev);
		if (ret) {
			pm_runtime_put(&cfg->pdev->dev);
			dev_err(&cfg->pdev->dev,
				"pm runtime get sync err, ret = %d\n",
				ret);
			return ret;
		}
		cfg->suspend_stage &= ~PAM_IPA_FORCE_SUSPEND;
	}

	if (cfg->suspend_stage & PAM_IPA_EB_SUSPEND) {
		pam_ipa_set_enabled(cfg, true);
		cfg->suspend_stage &= ~PAM_IPA_EB_SUSPEND;
	}

	if (!(cfg->suspend_stage & PAM_IPA_REG_SUSPEND))
		goto out;

	if (!cfg->hal_ops.get_start_status(cfg->reg_base)) {
		if (!cfg->ready) {
			dev_err(&cfg->pdev->dev,
				"pam ipa not ready\n");
			return -EINPROGRESS;
		}

		ret = sipa_pam_connect(&cfg->pam_local_param);
		if (ret) {
			dev_err(&cfg->pdev->dev,
				"local ipa connect failed\n");
			return ret;
		}

		ret = pam_ipa_init(cfg);
		if (ret) {
			dev_err(&cfg->pdev->dev,
				"PAM_IPA init hw failed\n");
			return ret;
		}
	} else {
		ret = sipa_pam_connect(&cfg->pam_local_param);
		if (ret) {
			dev_err(&cfg->pdev->dev,
				"local ipa connect failed\n");
			return ret;
		}

		cfg->hal_ops.resume(cfg->reg_base, true);
	}

	cfg->suspend_stage &= ~PAM_IPA_REG_SUSPEND;

out:
	sipa_rm_notify_completion(SIPA_RM_EVT_GRANTED,
				  SIPA_RM_RES_PROD_PAM_IPA);

	return 0;
}

static void pam_ipa_power_work(struct work_struct *work)
{
	struct pam_ipa_cfg_tag *cfg = container_of((struct delayed_work *)work,
						   struct pam_ipa_cfg_tag,
						   power_work);

	if (cfg->power_status) {
		if (pam_ipa_prepare_resume(&cfg->pdev->dev) &&
		    cfg->power_status) {
			dev_warn(&cfg->pdev->dev,
				 "pam ipa resume fail, resume again\n");
			queue_delayed_work(cfg->power_wq, &cfg->power_work,
					   msecs_to_jiffies(200));
		}
	} else {
		pam_ipa_prepare_suspend(&cfg->pdev->dev);
	}
}

static int pam_ipa_req_res(void *priv)
{
	struct pam_ipa_cfg_tag *cfg = dev_get_drvdata((struct device *)priv);

	cfg->power_status = true;

	cancel_delayed_work(&cfg->power_work);
	queue_delayed_work(cfg->power_wq, &cfg->power_work, 0);

	return -EINPROGRESS;
}

static int pam_ipa_rel_res(void *priv)
{
	struct pam_ipa_cfg_tag *cfg = dev_get_drvdata((struct device *)priv);

	cfg->power_status = false;

	cancel_delayed_work(&cfg->power_work);
	queue_delayed_work(cfg->power_wq, &cfg->power_work, 0);

	return 0;
}

static int pam_ipa_create_prod(struct device *dev)
{
	int ret;
	struct sipa_rm_create_params rm_params = {};

	/* create prod */
	rm_params.name = SIPA_RM_RES_PROD_PAM_IPA;
	rm_params.floor_voltage = 0;
	rm_params.reg_params.notify_cb = NULL;
	rm_params.reg_params.user_data = dev;
	rm_params.request_resource = pam_ipa_req_res;
	rm_params.release_resource = pam_ipa_rel_res;
	ret = sipa_rm_create_resource(&rm_params);
	if (ret)
		return ret;

	/* add dependencys */
	ret = sipa_rm_add_dependency(SIPA_RM_RES_CONS_WWAN_UL,
				     SIPA_RM_RES_PROD_PAM_IPA);
	if (ret < 0 && ret != -EINPROGRESS) {
		dev_err(dev, "pam ipa add_dependency WWAN_UL fail.\n");
		sipa_rm_delete_resource(SIPA_RM_RES_PROD_PAM_IPA);

		return ret;
	}

	ret = sipa_rm_add_dependency(SIPA_RM_RES_CONS_WWAN_DL,
				     SIPA_RM_RES_PROD_PAM_IPA);
	if (ret < 0 && ret != -EINPROGRESS) {
		dev_err(dev, "pam ipa add_dependency WWAN_DL fail.\n");
		sipa_rm_delete_dependency(SIPA_RM_RES_CONS_WWAN_UL,
					  SIPA_RM_RES_PROD_PAM_IPA);
		sipa_rm_delete_resource(SIPA_RM_RES_PROD_PAM_IPA);

		return ret;
	}

	return 0;
}

static int pam_ipa_plat_drv_probe(struct platform_device *pdev_p)
{
	int ret;
	struct pam_ipa_cfg_tag *cfg;

	cfg = devm_kzalloc(&pdev_p->dev, sizeof(*cfg),
			   GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;

	pam_ipa_cfg = cfg;
	cfg->pdev = pdev_p;
	ret = pam_ipa_parse_dts_configuration(pdev_p, cfg);
	if (ret)
		return ret;

	cfg->ready = false;
	cfg->pcie_offset = PAM_IPA_STI_64BIT(PAM_IPA_DDR_MAP_OFFSET_L,
					     PAM_IPA_DDR_MAP_OFFSET_H);
	cfg->pcie_rc_base = PAM_IPA_STI_64BIT(PAM_IPA_PCIE_RC_BASE_L,
					      PAM_IPA_PCIE_RC_BASE_H);
	cfg->suspend_stage = PAM_IPA_SUSPEND_STAGE;

	pam_ipa_init_api(&cfg->hal_ops);
	dev_set_drvdata(&pdev_p->dev, cfg);
	ret = pam_ipa_create_prod(&pdev_p->dev);
	if (ret)
		return ret;

	INIT_DELAYED_WORK(&cfg->power_work, pam_ipa_power_work);

	cfg->power_wq = create_workqueue("pam_ipa_power_wq");
	if (!cfg->power_wq) {
		dev_err(&pdev_p->dev, "pam ipa power wq create failed\n");
		return -ENOMEM;
	}

	pm_runtime_enable(&pdev_p->dev);

	return 0;
}

static int pam_ipa_ap_suspend(struct device *dev)
{
	return 0;
}

static int pam_ipa_ap_resume(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops pam_ipa_pm_ops = {
	.suspend_noirq = pam_ipa_ap_suspend,
	.resume_noirq = pam_ipa_ap_resume,
};

static struct platform_driver pam_ipa_plat_drv = {
	.probe = pam_ipa_plat_drv_probe,
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &pam_ipa_pm_ops,
		.of_match_table = pam_ipa_plat_drv_match,
	},
};

static int __init pam_ipa_module_init(void)
{
	pr_debug("PAM-IPA module init\n");

	/* Register as a platform device driver */
	return platform_driver_register(&pam_ipa_plat_drv);
}

static void __exit pam_ipa_module_exit(void)
{
	pr_debug("PAM-IPA module exit\n");

	destroy_workqueue(pam_ipa_cfg->power_wq);
	platform_driver_unregister(&pam_ipa_plat_drv);
}

module_init(pam_ipa_module_init);
module_exit(pam_ipa_module_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Spreadtrum PAM IPA HW device driver");

