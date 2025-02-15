
#include "main.h"


RCC_ClocksTypeDef  rccClocks;


/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL4;
  RCC_OscInitStruct.PLL.PLLDIV = RCC_PLL_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    ErrHandler( STOP );
  }
  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    ErrHandler( STOP );
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_RTC;
  PeriphClkInit.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    ErrHandler( STOP );
  }

  rccClocks.SYSCLK_Frequency = HAL_RCC_GetSysClockFreq();
  rccClocks.HCLK_Frequency = HAL_RCC_GetHCLKFreq();
  rccClocks.PCLK1_Frequency = HAL_RCC_GetPCLK1Freq();
  rccClocks.PCLK2_Frequency = HAL_RCC_GetPCLK2Freq();
}


//  Переключаем тактирование на MSI
void rccMsiSw( void ){
  // MSI = 4.194MHz
  RCC->ICSCR = (RCC->ICSCR & ~RCC_ICSCR_MSIRANGE) | RCC_ICSCR_MSIRANGE_4;
  RCC->CR |= RCC_CR_MSION;
  while( (RCC->CR & RCC_CR_MSIRDY) == 0 )
  {}
  RCC->CFGR &= ~RCC_CFGR_SW;
  while( (RCC->CFGR & RCC_CFGR_SWS) != 0 )
  {}
//  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);
  // Power range 3
  while( (PWR->CSR & PWR_CSR_VOSF) != 0 )
  {}
  PWR->CR = (PWR->CR & ~PWR_CR_VOS) | PWR_CR_VOS_0;
  while( (PWR->CSR & PWR_CSR_VOSF) != 0 )
  {}
  RCC->CR &= ~(RCC_CR_PLLON | RCC_CR_HSION);
}


//  Переключаем тактирование на HSI
void rccHsiSw( void ){
//  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  // Power range 1
  while( (PWR->CSR & PWR_CSR_VOSF) != 0 )
  {}
  PWR->CR = (PWR->CR & ~PWR_CR_VOS) | PWR_CR_VOS_0;
  while( (PWR->CSR & PWR_CSR_VOSF) != 0 )
  {}

  RCC->CR |= RCC_CR_HSION;
  while( (RCC->CR & RCC_CR_HSIRDY) == 0 )
  {}
  RCC->CR |= RCC_CR_PLLON;
  while( (RCC->CR & RCC_CR_PLLRDY) == 0)
  {}
  RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
  while( (RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL )
  {}
  RCC->CR &= ~RCC_CR_MSION;
}


