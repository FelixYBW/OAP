package org.apache.spark.network.pmof

import org.apache.spark.SparkConf

class RdmaClientPool(conf: SparkConf, poolSize: Int, address: String, port: Int) {
  val RdmaClients = new Array[RdmaClient](poolSize)

  init()

  def init(): Unit = {
    for (i <- 0 until poolSize) {
      RdmaClients(i) = new RdmaClient(conf, address, port)
      RdmaClients(i).init()
      RdmaClients(i).start()
    }
  }

  def get(index: Int): RdmaClient = {
    RdmaClients(index)
  }

  def stop(): Unit = {
    RdmaClients.foreach(_.stop())
  }

  def waitToStop(): Unit = {
    RdmaClients.foreach(_.waitToStop())
  }
}
