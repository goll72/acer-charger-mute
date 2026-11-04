// SPDX-License-Identifier: GPL-2.0

/*
 * Suppress the Acer charger connect/disconnect beep by temporarily
 * muting the Conexant speaker amplifier.
 *
 * Intercepts ACPI EC query 0x38, mutes the speaker amp, then evaluates
 * the original firmware _Q38 AML method to preserve its side effects.
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

#include <sound/hda_codec.h>
#include <sound/hda_regmap.h>
#include <sound/hda_verbs.h>

/*
 * ACPI EC query-handler interfaces.
 *
 * These are exported by drivers/acpi/ec.c but their declarations are
 * currently in drivers/acpi/internal.h, not include/linux/acpi.h.
 */
struct acpi_ec;

typedef int (*acer_ec_query_func_t)(void *data);

extern struct acpi_ec *first_ec;

extern int acpi_ec_add_query_handler(struct acpi_ec *ec,
                                     u8 query_bit,
                                     acpi_handle handle,
                                     acer_ec_query_func_t func,
                                     void *data);

extern void acpi_ec_remove_query_handler(struct acpi_ec *ec,
                                         u8 query_bit);


#define ACER_CODEC_VENDOR_ID    0x14f120d1
#define ACER_SPEAKER_NID        0x11
#define ACER_EC_QUERY           0x38

#define AMP_MUTE_BIT            0x80

#define RESTORE_RETRY_MS        1000


static unsigned int mute_ms = 120;
module_param(mute_ms, uint, 0644);
MODULE_PARM_DESC(mute_ms, "Speaker mute duration in milliseconds");

static unsigned int mute_delay_ms = 210;
module_param(mute_delay_ms, uint, 0644);
MODULE_PARM_DESC(mute_delay_ms,
                 "Delay before muting in milliseconds");

static bool enabled = true;
module_param(enabled, bool, 0644);
MODULE_PARM_DESC(enabled, "Enable charger-event speaker muting");


static struct device *codec_dev;
static struct hda_codec *codec;

static struct acpi_ec *ec;
static acpi_handle q38_handle;

static bool ec_handler_registered;

static DEFINE_MUTEX(amp_mutex);

static bool amp_muted;
static u8 saved_amp[2];

static struct delayed_work mute_work;
static struct delayed_work restore_work;


/*
 * Read the actual hardware amplifier value, bypassing the HDA regmap cache.
 *
 * The response is an 8-bit amplifier value:
 *
 *   bit 7: mute
 *   bits 6:0: gain
 */
static int read_amp(struct hda_codec *codec,
                    unsigned int channel,
                    u8 *value)
{
    unsigned int parm;
    unsigned int response;

    parm = AC_AMP_GET_OUTPUT |
           (channel ? AC_AMP_GET_RIGHT : AC_AMP_GET_LEFT);

    response = snd_hda_codec_read(codec,
                                  ACER_SPEAKER_NID,
                                  0,
                                  AC_VERB_GET_AMP_GAIN_MUTE,
                                  parm);

    if (response == (unsigned int)-1)
        return -EIO;

    *value = response & 0xff;

    return 0;
}


/*
 * Force a raw amplifier-register write.
 *
 * Unlike snd_hda_codec_amp_update(), this does not use an
 * update-if-changed operation to decide whether to issue the write.
 *
 * The raw regmap write also updates the regmap cache.
 *
 * Read back the hardware value to verify that the codec accepted it.
 */
static int write_amp(struct hda_codec *codec,
                     unsigned int channel,
                     u8 value)
{
    unsigned int reg;
    u8 readback;
    int ret;

    reg = snd_hdac_regmap_encode_amp(ACER_SPEAKER_NID,
                                     channel,
                                     HDA_OUTPUT,
                                     0);

    ret = snd_hdac_regmap_write_raw(&codec->core, reg, value);
    if (ret < 0)
        return ret;

    ret = read_amp(codec, channel, &readback);
    if (ret < 0)
        return ret;

    if (readback != value) {
        dev_err(codec_dev,
                "NID 0x%02x channel %u: write 0x%02x, read back 0x%02x\n",
                ACER_SPEAKER_NID, channel, value, readback);
        return -EIO;
    }

    return 0;
}


static int read_amp_pair(struct hda_codec *codec, u8 values[2])
{
    int ret;

    ret = read_amp(codec, 0, &values[0]);
    if (ret < 0)
        return ret;

    ret = read_amp(codec, 1, &values[1]);
    if (ret < 0)
        return ret;

    return 0;
}


/*
 * Write both channels.
 *
 * If rollback_values is non-NULL and a write fails, restore all channels
 * attempted so far to their previous values.
 */
static int write_amp_pair(struct hda_codec *codec,
                          const u8 values[2],
                          const u8 *rollback_values)
{
    int channel;
    int ret;

    for (channel = 0; channel < 2; channel++) {
        ret = write_amp(codec, channel, values[channel]);
        if (ret < 0) {
            int i;

            if (rollback_values) {
                for (i = channel; i >= 0; i--) {
                    int rollback_ret;

                    rollback_ret = write_amp(codec, i,
                                              rollback_values[i]);

                    if (rollback_ret < 0)
                        dev_err(codec_dev,
                                "Failed to roll back speaker amp channel %d: %d\n",
                                i, rollback_ret);
                }
            }

            return ret;
        }
    }

    return 0;
}


/* --------------------------------------------------------------------------
 * Mute and restore operations
 * -------------------------------------------------------------------------- */

/*
 * amp_mutex must be held by the caller.
 */
static int mute_speaker_locked(void)
{
    const u8 mute_values[2] = {
        AMP_MUTE_BIT,
        AMP_MUTE_BIT,
    };

    int ret;

    if (amp_muted)
        return 0;

    device_lock(codec_dev);

    if (!codec_dev->driver) {
        ret = -ENODEV;
        goto out_unlock;
    }

    /*
     * Save the actual hardware values, not the potentially stale
     * register-cache values.
     */
    ret = read_amp_pair(codec, saved_amp);
    if (ret < 0) {
        dev_err(codec_dev, "Failed to read speaker amplifier state: %d\n",
                ret);
        goto out_unlock;
    }

    dev_info(codec_dev,
             "Saving speaker amp state: left=0x%02x right=0x%02x\n",
             saved_amp[0], saved_amp[1]);

    /*
     * Mute and set gain to zero on both channels.
     * If either write fails, attempt to restore the original values.
     */
    ret = write_amp_pair(codec, mute_values, saved_amp);
    if (ret < 0) {
        dev_err(codec_dev, "Failed to mute speaker amplifier: %d\n", ret);
        goto out_unlock;
    }

    amp_muted = true;

    dev_info(codec_dev, "Speaker amplifier muted\n");

out_unlock:
    device_unlock(codec_dev);

    return ret;
}


/*
 * amp_mutex must be held by the caller.
 */
static int restore_speaker_locked(void)
{
    int ret;

    if (!amp_muted)
        return 0;

    device_lock(codec_dev);

    if (!codec_dev->driver) {
        ret = -ENODEV;
        goto out_unlock;
    }

    ret = write_amp_pair(codec, saved_amp, NULL);
    if (ret < 0) {
        dev_err(codec_dev,
                "Failed to restore speaker amplifier state: %d\n", ret);
        goto out_unlock;
    }

    amp_muted = false;

    dev_info(codec_dev,
             "Speaker amplifier restored: left=0x%02x right=0x%02x\n",
             saved_amp[0], saved_amp[1]);

out_unlock:
    device_unlock(codec_dev);

    return ret;
}


static void mute_work_fn(struct work_struct *work)
{
    unsigned long delay;
    int ret;

    mutex_lock(&amp_mutex);

    ret = mute_speaker_locked();

    if (!ret && amp_muted) {
        delay = msecs_to_jiffies(READ_ONCE(mute_ms));

        mod_delayed_work(system_wq, &restore_work, delay);
    }

    mutex_unlock(&amp_mutex);
}

static void charger_event(void)
{
    unsigned int delay_ms = READ_ONCE(mute_delay_ms);

    mutex_lock(&amp_mutex);

    if (!READ_ONCE(enabled)) {
        mutex_unlock(&amp_mutex);
        return;
    }

    if (amp_muted)
        mod_delayed_work(system_wq, &restore_work,
            msecs_to_jiffies(delay_ms + READ_ONCE(mute_ms)));

    mod_delayed_work(system_wq, &mute_work,
                     msecs_to_jiffies(delay_ms));

    mutex_unlock(&amp_mutex);
}


static void restore_work_fn(struct work_struct *work)
{
    int ret;

    mutex_lock(&amp_mutex);

    ret = restore_speaker_locked();

    /*
     * Retry restoration if the codec is temporarily unavailable.
     * This also prevents a transient error from leaving the speaker
     * muted indefinitely.
     */
    if (ret < 0 && amp_muted) {
        pr_warn("acer_charger_mute: retrying speaker restoration in %u ms\n",
                RESTORE_RETRY_MS);

        mod_delayed_work(system_wq,
                         &restore_work,
                         msecs_to_jiffies(RESTORE_RETRY_MS));
    }

    mutex_unlock(&amp_mutex);
}


/*
 * The ACPI EC query-handler API inserts custom handlers ahead of the
 * automatically discovered AML _Qxx handlers.
 *
 * Consequently, registering this callback intercepts _Q38. We must
 * explicitly evaluate the original AML method to preserve its behavior.
 */
static int acer_ec_query_38(void *data)
{
    if (READ_ONCE(enabled))
        charger_event();

    /* Preserve the original firmware behavior */
    acpi_status status = acpi_evaluate_object(q38_handle, NULL, NULL, NULL);

    if (ACPI_FAILURE(status)) {
        pr_err("acer_charger_mute: failed to evaluate original _Q38: %s\n",
               acpi_format_exception(status));
    }

    return 0;
}


static int match_codec(struct device *dev, const void *data)
{
    const struct hdac_device *hdev = dev_to_hdac_dev(dev);
    const unsigned int *vendor_id = data;

    return hdev->vendor_id == *vendor_id;
}


static int find_codec(void)
{
    const unsigned int vendor_id = ACER_CODEC_VENDOR_ID;
    int ret;

    codec_dev = bus_find_device(&snd_hda_bus_type,
                                NULL,
                                &vendor_id,
                                match_codec);

    if (!codec_dev) {
        pr_err("acer_charger_mute: codec 0x%08x not found\n",
               ACER_CODEC_VENDOR_ID);
        return -ENODEV;
    }

    codec = dev_to_hda_codec(codec_dev);

    device_lock(codec_dev);

    if (!codec_dev->driver)
        ret = -ENODEV;
    else
        ret = 0;

    device_unlock(codec_dev);

    if (ret < 0) {
        dev_err(codec_dev, "HDA codec has no bound driver\n");
        put_device(codec_dev);
        codec_dev = NULL;
        codec = NULL;
        return ret;
    }

    dev_info(codec_dev, "Found codec 0x%08x\n",
             codec->core.vendor_id);

    return 0;
}


static int __init acer_charger_mute_init(void)
{
    acpi_handle ec_handle;
    acpi_status status;
    int ret;

    INIT_DELAYED_WORK(&mute_work, mute_work_fn);
    INIT_DELAYED_WORK(&restore_work, restore_work_fn);

    /*
     * Locate the system ACPI EC.
     *
     * first_ec is exported by drivers/acpi/ec.c, but is declared in
     * drivers/acpi/internal.h rather than a public header.
     */
    ec = first_ec;
    if (!ec) {
        pr_err("acer_charger_mute: ACPI EC is unavailable\n");
        return -ENODEV;
    }

    ec_handle = ec_get_handle();
    if (!ec_handle) {
        pr_err("acer_charger_mute: ACPI EC handle is unavailable\n");
        return -ENODEV;
    }

    /*
     * Find the original firmware _Q38 method. Do this before registering
     * our handler so we never intercept the query without being able
     * to forward it to the original AML method.
     */
    status = acpi_get_handle(ec_handle, "_Q38", &q38_handle);
    if (ACPI_FAILURE(status)) {
        pr_err("acer_charger_mute: unable to find EC._Q38: %s\n",
               acpi_format_exception(status));
        return -ENODEV;
    }

    ret = find_codec();
    if (ret < 0)
        return ret;

    /*
     * Register our handler for EC query 0x38.
     *
     * This shadows the automatically registered AML handler. Our
     * callback explicitly evaluates that AML method after muting.
     */
    ret = acpi_ec_add_query_handler(ec,
                                    ACER_EC_QUERY,
                                    NULL,
                                    acer_ec_query_38,
                                    NULL);

    if (ret < 0) {
        dev_err(codec_dev,
                "Failed to register EC query 0x%02x handler: %d\n",
                ACER_EC_QUERY, ret);

        put_device(codec_dev);
        codec_dev = NULL;
        codec = NULL;

        return ret;
    }

    ec_handler_registered = true;

    pr_info("acer_charger_mute: registered for ACPI EC query 0x%02x\n",
            ACER_EC_QUERY);

    return 0;
}


static void __exit acer_charger_mute_exit(void)
{
    int ret;

    /*
     * Remove the EC handler first. This also flushes the EC query
     * workqueue, ensuring that no callback is still running or can
     * schedule another restoration.
     */
    if (ec_handler_registered) {
        acpi_ec_remove_query_handler(ec, ACER_EC_QUERY);
        ec_handler_registered = false;
    }

    cancel_delayed_work_sync(&mute_work);
    cancel_delayed_work_sync(&restore_work);

    /*
     * Do not leave the speaker muted when unloading the module.
     */
    mutex_lock(&amp_mutex);

    ret = restore_speaker_locked();
    if (ret < 0)
        pr_err("acer_charger_mute: final speaker restoration failed: %d\n",
               ret);

    mutex_unlock(&amp_mutex);

    if (codec_dev) {
        put_device(codec_dev);
        codec_dev = NULL;
        codec = NULL;
    }

    pr_info("acer_charger_mute: unloaded\n");
}


module_init(acer_charger_mute_init);
module_exit(acer_charger_mute_exit);

MODULE_DESCRIPTION("Acer charger event speaker beep suppression");
MODULE_AUTHOR("goll");
MODULE_LICENSE("GPL");
