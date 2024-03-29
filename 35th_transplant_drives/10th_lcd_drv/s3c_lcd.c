/*
*
*
*/
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <linux/clk.h>

#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/div64.h>

#include <asm/mach/map.h>
//#include <asm/arch/regs-lcd.h>
//#include <asm/arch/regs-gpio.h>
//#include <asm/arch/fb.h>

static int major = 0;
static u32 pseudo_palette[16];

static struct fb_info *s3c_fb;
static struct class *s3c_fb_class;
static struct class_device *s3c_fb_class_device;

static int s3c_setcolreg(unsigned regno,
			       unsigned red, unsigned green, unsigned blue,
			       unsigned transp, struct fb_info *info);

static struct fb_ops s3c_lcd_fops = {
	.owner	= THIS_MODULE,
	.fb_setcolreg  	= s3c_setcolreg,
	.fb_fillrect 	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
};

struct lcd_regs {
	unsigned long	lcdcon1;
	unsigned long	lcdcon2;	
	unsigned long	lcdcon3;	
	unsigned long	lcdcon4;	
	unsigned long	lcdcon5;    
	unsigned long	lcdsaddr1;    
	unsigned long	lcdsaddr2;    
	unsigned long	lcdsaddr3;    
	unsigned long	redlut;    
	unsigned long	greenlut;    
	unsigned long	bluelut;    
	unsigned long	reserved[9];    
	unsigned long	dithmode;    
	unsigned long	tpal;    
	unsigned long	lcdintpnd;    
	unsigned long	lcdsrcpnd;    
	unsigned long	lcdintmsk;
	unsigned long	lpcsel;
};

static volatile struct lcd_regs* lcd_regs;
static volatile unsigned long *gpbcon;
static volatile unsigned long *gpbdat;
static volatile unsigned long *gpccon;
static volatile unsigned long *gpdcon;
static volatile unsigned long *gpgcon;


static inline unsigned int chan_to_field(unsigned int chan, struct fb_bitfield *bf)
{
	chan &= 0xffff;
	chan >>= 16 - bf->length;
	return chan << bf->offset;
}

static int s3c_setcolreg(unsigned regno,
			       unsigned red, unsigned green, unsigned blue,
			       unsigned transp, struct fb_info *info)
{
	unsigned int val;
	
	if (regno > 16)
		return 1;

	/* 用red,green,blue三原色构造出val */
	val  = chan_to_field(red,	&info->var.red);
	val |= chan_to_field(green, &info->var.green);
	val |= chan_to_field(blue,	&info->var.blue);
	
	//((u32 *)(info->pseudo_palette))[regno] = val;
	pseudo_palette[regno] = val;
	return 0;
}


static int s3c_fb_init(void)
{
	/*1.assgin a fb_info struct*/
	s3c_fb = framebuffer_alloc(0, NULL);
	if (!s3c_fb)
		return -ENOMEM;
	
	/*2.setup struce parameter*/
	strcpy(s3c_fb->fix.id, "mylcd");
	//s3c_fb->fix.smem_start 	= ;
	s3c_fb->fix.smem_len		= 480*272*16/8;
	s3c_fb->fix.type			= FB_TYPE_PACKED_PIXELS;
	s3c_fb->fix.visual			= FB_VISUAL_TRUECOLOR;  /*TFT 真彩色*/
	s3c_fb->fix.line_length		= 480*2;
	s3c_fb->fix.accel	    = FB_ACCEL_NONE;
	
	s3c_fb->var.xres			= 480;
	s3c_fb->var.xres_virtual	= 480;
	s3c_fb->var.yres			= 272;
	s3c_fb->var.yres_virtual	= 272;
	s3c_fb->var.bits_per_pixel	= 16;

	s3c_fb->var.red.offset      	= 11;
	s3c_fb->var.green.offset    	= 5;
	s3c_fb->var.blue.offset     	= 0;
	s3c_fb->var.transp.offset  	= 0;
	s3c_fb->var.red.length     	= 5;
	s3c_fb->var.green.length    = 6;
	s3c_fb->var.blue.length    	= 5;
	s3c_fb->var.transp.length   = 0;

	s3c_fb->fbops			= &s3c_lcd_fops;
	s3c_fb->pseudo_palette	= pseudo_palette;
	s3c_fb->screen_size		= 480*272*16/8;  /*byte show*/
	
	/*3.hardware correlation setup*/
	gpbcon = ioremap(0x56000010, 8);
	gpbdat = gpbcon+1;
	gpccon = ioremap(0x56000020, 4);
	gpdcon = ioremap(0x56000030, 4);
	gpgcon = ioremap(0x56000060, 4);

   	*gpccon  = 0xaaaaaaaa;   /* GPIO管脚用于VD[7:0],LCDVF[2:0],VM,VFRAME,VLINE,VCLK,LEND */
	*gpdcon  = 0xaaaaaaaa;   /* GPIO管脚用于VD[23:8] */
	
	*gpbcon &= ~(3);  /* GPB0设置为输出引脚 */
	*gpbcon |= 1;
	*gpbdat &= ~1;     /* 输出低电平 */

	*gpgcon |= (3<<8); /* GPG4用作LCD_PWREN */
	
	/* 3.2 根据LCD手册设置LCD控制器, 比如VCLK的频率等 */
	lcd_regs = ioremap(0x4D000000, sizeof(struct lcd_regs));

	/* bit[17:8]: VCLK = HCLK / [(CLKVAL+1) x 2], LCD手册P14
	 *            10MHz(100ns) = 100MHz / [(CLKVAL+1) x 2]
	 *            CLKVAL = 4
	 * bit[6:5]: 0b11, TFT LCD
	 * bit[4:1]: 0b1100, 16 bpp for TFT
	 * bit[0]  : 0 = Disable the video output and the LCD control signal.
	 */
	lcd_regs->lcdcon1  = (4<<8) | (3<<5) | (0x0c<<1);

	/* 垂直方向的时间参数
	 * bit[31:24]: VBPD, VSYNC之后再过多长时间才能发出第1行数据
	 *             LCD手册 T0-T2-T1=4
	 *             VBPD=3
	 * bit[23:14]: 多少行, 320, 所以LINEVAL=320-1=319
	 * bit[13:6] : VFPD, 发出最后一行数据之后，再过多长时间才发出VSYNC
	 *             LCD手册T2-T5=322-320=2, 所以VFPD=2-1=1
	 * bit[5:0]  : VSPW, VSYNC信号的脉冲宽度, LCD手册T1=1, 所以VSPW=1-1=0
	 */
	lcd_regs->lcdcon2  = (3<<24) | (271<<14) | (1<<6) | (0<<0);


	/* 水平方向的时间参数
	 * bit[25:19]: HBPD, VSYNC之后再过多长时间才能发出第1行数据
	 *             LCD手册 T6-T7-T8=17
	 *             HBPD=16
	 * bit[18:8]: 多少列, 240, 所以HOZVAL=240-1=239
	 * bit[7:0] : HFPD, 发出最后一行里最后一个象素数据之后，再过多长时间才发出HSYNC
	 *             LCD手册T8-T11=251-240=11, 所以HFPD=11-1=10
	 */
	lcd_regs->lcdcon3 = (16<<19) | (479<<8) | (10<<0);

	/* 水平方向的同步信号
	 * bit[7:0]	: HSPW, HSYNC信号的脉冲宽度, LCD手册T7=5, 所以HSPW=5-1=4
	 */	
	lcd_regs->lcdcon4 = 4;

	/* 信号的极性 
	 * bit[11]: 1=565 format
	 * bit[10]: 0 = The video data is fetched at VCLK falling edge
	 * bit[9] : 1 = HSYNC信号要反转,即低电平有效 
	 * bit[8] : 1 = VSYNC信号要反转,即低电平有效 
	 * bit[6] : 0 = VDEN不用反转
	 * bit[3] : 0 = PWREN输出0
	 * bit[1] : 0 = BSWP
	 * bit[0] : 1 = HWSWP 2440手册P413
	 */
	lcd_regs->lcdcon5 = (1<<11) | (0<<10) | (1<<9) | (1<<8) | (1<<0);
	
	/* 3.3 分配显存(framebuffer), 并把地址告诉LCD控制器 */
	s3c_fb->screen_base = dma_alloc_writecombine(NULL, s3c_fb->fix.smem_len, &s3c_fb->fix.smem_start, GFP_KERNEL);
	
	lcd_regs->lcdsaddr1  = (s3c_fb->fix.smem_start >> 1) & ~(3<<30);
	lcd_regs->lcdsaddr2  = ((s3c_fb->fix.smem_start + s3c_fb->fix.smem_len) >> 1) & 0x1fffff;
	lcd_regs->lcdsaddr3  = (480*16/16);  /* 一行的长度(单位: 2字节) */	
	
	//s3c_fb->fix.smem_start = xxx;  /* 显存的物理地址 */
	/* 启动LCD */
	lcd_regs->lcdcon1 |= (1<<0); /* 使能LCD控制器 */
	lcd_regs->lcdcon5 |= (1<<3); /* 使能LCD本身 */
	
	*gpbdat |= 1;     /* 输出高电平, 使能背光 */

	printk("smem_start = %08x\n",s3c_fb->fix.smem_start);
	
	/*4.register fb_info struct*/
	register_framebuffer(s3c_fb);
	
	return 0;
}

static void s3c_fb_exit(void)
{
	unregister_framebuffer(s3c_fb);
	lcd_regs->lcdcon1 &= ~(1<<0); /* 关闭LCD本身 */
	*gpbdat &= ~1;     /* 关闭背光 */
	dma_free_writecombine(NULL, s3c_fb->fix.smem_len, s3c_fb->screen_base, s3c_fb->fix.smem_start);
	iounmap(lcd_regs);
	iounmap(gpbcon);
	iounmap(gpccon);
	iounmap(gpdcon);
	iounmap(gpgcon);
	framebuffer_release(s3c_fb);
}

module_init(s3c_fb_init);
module_exit(s3c_fb_exit);
MODULE_LICENSE("GPL");
